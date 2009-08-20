#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <ccn/ccn.h>
#include <ccn/ccn_private.h>
#include <ccn/ccnd.h>
#include <ccn/charbuf.h>
#include <ccn/coding.h>
#include <ccn/digest.h>
#include <ccn/hashtb.h>
#include <ccn/signing.h>

struct ccn {
    int sock;
    size_t outbufindex;
    struct ccn_charbuf *interestbuf;
    struct ccn_charbuf *inbuf;
    struct ccn_charbuf *outbuf;
    struct hashtb *interests_by_prefix;
    struct hashtb *interest_filters;
    struct ccn_skeleton_decoder decoder;
    struct ccn_indexbuf *scratch_indexbuf;
    struct hashtb *keys;	/* KEYS */
    struct timeval now;
    int timeout;
    int refresh_us;
    int err;                    /* pos => errno value, neg => other */
    int errline;
    int verbose_error;
    int tap;
    int running;
};

struct expressed_interest;

struct interests_by_prefix { /* keyed by components of name prefix */
    struct expressed_interest *list;
};

struct expressed_interest {
    int magic;                   /* for sanity checking */
    struct timeval lasttime;     /* time most recently expressed */
    struct ccn_closure *action;  /* handler for incoming content */
    unsigned char *interest_msg; /* the interest message as sent */
    size_t size;                 /* its size in bytes */
    int target;                  /* how many we want outstanding (0 or 1) */
    int outstanding;             /* number currently outstanding (0 or 1) */
    struct ccn_charbuf *wanted_pub; /* waiting for this pub to arrive */
    struct expressed_interest *next; /* link to next in list */
};

struct interest_filter { /* keyed by components of name */
    struct ccn_closure *action;
};

#define NOTE_ERR(h, e) (h->err = (e), h->errline = __LINE__, ccn_note_err(h))
#define NOTE_ERRNO(h) NOTE_ERR(h, errno)

#define THIS_CANNOT_HAPPEN(h) \
    do { NOTE_ERR(h, -73); ccn_perror(h, "Can't happen");} while (0)

#define XXX \
    do { NOTE_ERR(h, -76); ccn_perror(h, "Please write some more code here"); } while (0)

static void ccn_refresh_interest(struct ccn *, struct expressed_interest *);

/**
 * Produce message on standard error output describing the last
 * error encountered during a call using the given handle.
 * @param h is the ccn handle - may not be NULL.
 * @param s is a client-supplied message; if NULL a message will be supplied
 *        where available.
 */
void
ccn_perror(struct ccn *h, const char *s)
{
    const char *dlm = ": ";
    if (s == NULL) {
        if (h->err > 0)
            s = strerror(h->err);
        else
            dlm = s = "";
    }
    // XXX - time stamp
    fprintf(stderr, "ccn_client.c:%d[%d] - error %d%s%s\n",
                        h->errline, (int)getpid(), h->err, dlm, s);
}

static int
ccn_note_err(struct ccn *h)
{
    if (h->verbose_error)
        ccn_perror(h, NULL);
    return(-1);
}

static struct ccn_indexbuf *
ccn_indexbuf_obtain(struct ccn *h)
{
    struct ccn_indexbuf *c = h->scratch_indexbuf;
    if (c == NULL)
        return(ccn_indexbuf_create());
    h->scratch_indexbuf = NULL;
    c->n = 0;
    return(c);
}

static void
ccn_indexbuf_release(struct ccn *h, struct ccn_indexbuf *c)
{
    c->n = 0;
    if (h->scratch_indexbuf == NULL)
        h->scratch_indexbuf = c;
    else
        ccn_indexbuf_destroy(&c);
}

static void
ccn_replace_handler(struct ccn *h,
                    struct ccn_closure **dstp,
                    struct ccn_closure *src)
{
    struct ccn_closure *old = *dstp;
    if (src == old)
        return;
    if (src != NULL)
        src->refcount++;
    *dstp = src;
    if (old != NULL && (--(old->refcount)) == 0) {
        struct ccn_upcall_info info = { 0 };
        info.h = h;
        (old->p)(old, CCN_UPCALL_FINAL, &info);
    }
}

/**
 * Create a client handle.
 * The new handle is not yet connected.
 * On error, returns NULL and sets errno.
 * Errors: ENOMEM
 */ 
struct ccn *
ccn_create(void)
{
    struct ccn *h;
    const char *s;

    h = calloc(1, sizeof(*h));
    if (h == NULL)
        return(h);
    h->sock = -1;
    h->interestbuf = ccn_charbuf_create();
    h->keys = hashtb_create(sizeof(struct ccn_pkey *), NULL);
    s = getenv("CCN_DEBUG");
    h->verbose_error = (s != NULL && s[0] != 0);
    s = getenv("CCN_TAP");
    if (s != NULL && s[0] != 0) {
	char tap_name[255];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (snprintf(tap_name, 255, "%s-%d-%d-%d", s, (int)getpid(), (int)tv.tv_sec, (int)tv.tv_usec) >= 255) {
	    fprintf(stderr, "CCN_TAP path is too long: %s\n", s);
	} else {
	    h->tap = open(tap_name, O_WRONLY|O_APPEND|O_CREAT, S_IRWXU);
	    if (h->tap == -1) {
		NOTE_ERRNO(h);
                ccn_perror(h, "Unable to open CCN_TAP file");
	    }
            else
		fprintf(stderr, "CCN_TAP writing to %s\n", tap_name);
	}
    } else {
	h->tap = -1;
    }
    return(h);
}

/**
 * Connect to local ccnd.
 * @param h is a ccn library handle
 * @param name is the name of the unix-domain socket to connect to;
 *             use NULL to get the default.
 * @returns the fd for the connection, or -1 for error.
 */ 
int
ccn_connect(struct ccn *h, const char *name)
{
    struct sockaddr_un addr = {0};
    int res;
    char name_buf[60];
    if (h == NULL)
        return(-1);
    h->err = 0;
    if (h->sock != -1)
        return(NOTE_ERR(h, EINVAL));
    if (name == NULL || name[0] == 0) {
        name = getenv(CCN_LOCAL_PORT_ENVNAME);
        if (name == NULL || name[0] == 0 || strlen(name) > 10) {
            name = CCN_DEFAULT_LOCAL_SOCKNAME;
        }
        else {
            snprintf(name_buf, sizeof(name_buf), "%s.%s",
                     CCN_DEFAULT_LOCAL_SOCKNAME, name);
            name = name_buf;
        }
    }
    h->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (h->sock == -1)
        return(NOTE_ERRNO(h));
    strncpy(addr.sun_path, name, sizeof(addr.sun_path));
    addr.sun_family = AF_UNIX;
    res = connect(h->sock, (struct sockaddr *)&addr, sizeof(addr));
    if (res == -1)
        return(NOTE_ERRNO(h));
    res = fcntl(h->sock, F_SETFL, O_NONBLOCK);
    if (res == -1)
        return(NOTE_ERRNO(h));
    return(h->sock);
}

int
ccn_get_connection_fd(struct ccn *h)
{
    return(h->sock);
}

int
ccn_disconnect(struct ccn *h)
{
    int res;
    ccn_charbuf_destroy(&h->inbuf);
    ccn_charbuf_destroy(&h->outbuf);
    res = close(h->sock);
    h->sock = -1;
    if (res == -1)
        return(NOTE_ERRNO(h));
    return(0);
}

static void
ccn_gripe(struct expressed_interest *i)
{
    fprintf(stderr, "BOTCH - (struct expressed_interest *)%p has bad magic value\n", (void *)i);
}

static void
replace_interest_msg(struct expressed_interest *interest,
                     struct ccn_charbuf *cb)
{
    if (interest->magic != 0x7059e5f4) {
        ccn_gripe(interest);
        return;
    }
    if (interest->interest_msg != NULL)
        free(interest->interest_msg);
    interest->interest_msg = NULL;
    interest->size = 0;
    if (cb != NULL && cb->length > 0) {
        interest->interest_msg = calloc(1, cb->length);
        if (interest->interest_msg != NULL) {
            memcpy(interest->interest_msg, cb->buf, cb->length);
            interest->size = cb->length;
        }
    }
}

static struct expressed_interest *
ccn_destroy_interest(struct ccn *h, struct expressed_interest *i)
{
    struct expressed_interest *ans = i->next;
    if (i->magic != 0x7059e5f4) {
        ccn_gripe(i);
        return(NULL);
    }
    ccn_replace_handler(h, &(i->action), NULL);
    replace_interest_msg(i, NULL);
    ccn_charbuf_destroy(&i->wanted_pub);
    i->magic = -1;
    free(i);
    return(ans);
}

void
ccn_check_interests(struct expressed_interest *list)
{
    struct expressed_interest *ie;
    for (ie = list; ie != NULL; ie = ie->next) {
        if (ie->magic != 0x7059e5f4) {
            ccn_gripe(ie);
            abort();
        }
    }
}

void
ccn_clean_interests_by_prefix(struct ccn *h, struct interests_by_prefix *entry)
{
    struct expressed_interest *ie;
    struct expressed_interest *next;
    struct expressed_interest **ip;
    ccn_check_interests(entry->list);
    ip = &(entry->list);
    for (ie = entry->list; ie != NULL; ie = next) {
        next = ie->next;
        if (ie->action == NULL)
            ccn_destroy_interest(h, ie);
        else {
            (*ip) = ie;
            ip = &(ie->next);
        }
    }
    (*ip) = NULL;
    ccn_check_interests(entry->list);
}

void
ccn_destroy(struct ccn **hp)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn *h = *hp;
    if (h == NULL)
        return;
    ccn_disconnect(h);
    if (h->interests_by_prefix != NULL) {
        for (hashtb_start(h->interests_by_prefix, e); e->data != NULL; hashtb_next(e)) {
            struct interests_by_prefix *entry = e->data;
            while (entry->list != NULL)
                entry->list = ccn_destroy_interest(h, entry->list);
        }
        hashtb_end(e);
        hashtb_destroy(&(h->interests_by_prefix));
    }
    if (h->interest_filters != NULL) {
        for (hashtb_start(h->interest_filters, e); e->data != NULL; hashtb_next(e)) {
            struct interest_filter *i = e->data;
            ccn_replace_handler(h, &(i->action), NULL);
        }
        hashtb_end(e);
        hashtb_destroy(&(h->interest_filters));
    }

    /* XXX: remove this and rewrite as a finalizer on the hash table */
    if (h->keys != NULL) {	/* KEYS */
        for (hashtb_start(h->keys, e); e->data != NULL; hashtb_next(e)) {
            struct ccn_pkey **entry = e->data;
            if (*entry != NULL)
                ccn_pubkey_free(*entry);
            *entry = NULL;
        }
        hashtb_end(e);
        hashtb_destroy(&(h->keys));
    }
    ccn_charbuf_destroy(&h->interestbuf);
    ccn_indexbuf_destroy(&h->scratch_indexbuf);
    if (h->tap != -1) {
	close(h->tap);
    }
    free(h);
    *hp = NULL;
}

/*
 * ccn_check_namebuf: check that name is valid
 * Returns the byte offset of the end of prefix portion,
 * as given by prefix_comps, or -1 for error.
 * prefix_comps = -1 means the whole name is the prefix.
 * If omit_possible_digest, chops off a potential digest name at the end
 */
static int
ccn_check_namebuf(struct ccn *h, struct ccn_charbuf *namebuf, int prefix_comps,
                  int omit_possible_digest)
{
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;
    int i = 0;
    int ans = 0;
    int prev_ans = 0;
    if (namebuf == NULL || namebuf->length < 2)
        return(-1);
    d = ccn_buf_decoder_start(&decoder, namebuf->buf, namebuf->length);
    if (ccn_buf_match_dtag(d, CCN_DTAG_Name)) {
        ccn_buf_advance(d);
        prev_ans = ans = d->decoder.token_index;
        while (ccn_buf_match_dtag(d, CCN_DTAG_Component)) {
            ccn_buf_advance(d);
            if (ccn_buf_match_blob(d, NULL, NULL)) {
                ccn_buf_advance(d);
            }
            ccn_buf_check_close(d);
            i += 1;
            if (prefix_comps < 0 || i <= prefix_comps) {
                prev_ans = ans;
                ans = d->decoder.token_index;
            }
        }
        ccn_buf_check_close(d);
    }
    if (d->decoder.state < 0 || ans < prefix_comps)
        return(-1);
    if (omit_possible_digest && ans == prev_ans + 36 && ans == namebuf->length - 1)
        return(prev_ans);
    return(ans);
}

static void
ccn_construct_interest(struct ccn *h,
                       struct ccn_charbuf *namebuf,
                       int prefix_comps,
                       struct ccn_charbuf *interest_template,
                       struct expressed_interest *dest)
{
    struct ccn_charbuf *c = h->interestbuf;
    size_t start;
    size_t size;
    int res;
    char buf[20];
    
    c->length = 0;
    ccn_charbuf_append_tt(c, CCN_DTAG_Interest, CCN_DTAG);
    ccn_charbuf_append(c, namebuf->buf, namebuf->length);
    if (prefix_comps >= 0) {
        ccn_charbuf_append_tt(c, CCN_DTAG_NameComponentCount, CCN_DTAG);
        res = snprintf(buf, sizeof(buf), "%d", prefix_comps);
        ccn_charbuf_append_tt(c, res, CCN_UDATA);
        ccn_charbuf_append(c, buf, res);
        ccn_charbuf_append_closer(c);
    }
    res = 0;
    if (interest_template != NULL) {
        struct ccn_parsed_interest pi = { 0 };
        res = ccn_parse_interest(interest_template->buf,
                                 interest_template->length, &pi, NULL);
        if (res >= 0) {
            start = pi.offset[CCN_PI_E_NameComponentCount];
            size = pi.offset[CCN_PI_B_Nonce] - start;
            ccn_charbuf_append(c, interest_template->buf + start, size);
            start = pi.offset[CCN_PI_B_OTHER];
            size = pi.offset[CCN_PI_E_OTHER] - start;
            if (size != 0)
                ccn_charbuf_append(c, interest_template->buf + start, size);
        }
        else
            NOTE_ERR(h, EINVAL);
    }
    ccn_charbuf_append_closer(c);
    replace_interest_msg(dest, (res >= 0 ? c : NULL));
}

int
ccn_express_interest(struct ccn *h,
                     struct ccn_charbuf *namebuf,
                     int prefix_comps,
                     struct ccn_closure *action,
                     struct ccn_charbuf *interest_template)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    int prefixend;
    struct expressed_interest *interest = NULL;
    struct interests_by_prefix *entry = NULL;
    if (h->interests_by_prefix == NULL) {
        h->interests_by_prefix = hashtb_create(sizeof(struct interests_by_prefix), NULL);
        if (h->interests_by_prefix == NULL)
            return(NOTE_ERRNO(h));
    }
    prefixend = ccn_check_namebuf(h, namebuf, prefix_comps, 1);
    if (prefixend < 0)
        return(prefixend);
    /*
     * To make it easy to lookup prefixes of names, we keep only
     * the prefix name components as the key in the hash table.
     */
    hashtb_start(h->interests_by_prefix, e);
    res = hashtb_seek(e, namebuf->buf + 1, prefixend - 1, 0);
    entry = e->data;
    if (entry == NULL) {
        NOTE_ERRNO(h);
        hashtb_end(e);
        return(res);
    }
    if (res == HT_NEW_ENTRY)
        entry->list = NULL;
    interest = calloc(1, sizeof(*interest));
    if (interest == NULL) {
        NOTE_ERRNO(h);
        hashtb_end(e);
        return(-1);
    }
    interest->magic = 0x7059e5f4;
    ccn_construct_interest(h, namebuf, prefix_comps, interest_template, interest);
    if (interest->interest_msg == NULL) {
        free(interest);
        hashtb_end(e);
        return(-1);
    }
    ccn_replace_handler(h, &(interest->action), action);
    interest->target = 1;
    interest->next = entry->list;
    entry->list = interest;
    hashtb_end(e);
    /* Actually send the interest out right away */
    ccn_refresh_interest(h, interest);
    return(0);
}

int
ccn_set_interest_filter(struct ccn *h, struct ccn_charbuf *namebuf,
                        struct ccn_closure *action)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    struct interest_filter *entry;
    if (h->interest_filters == NULL) {
        h->interest_filters = hashtb_create(sizeof(struct interest_filter), NULL);
        if (h->interest_filters == NULL)
            return(NOTE_ERRNO(h));
    }
    res = ccn_check_namebuf(h, namebuf, -1, 0);
    if (res < 0)
        return(res);
    hashtb_start(h->interest_filters, e);
    res = hashtb_seek(e, namebuf->buf + 1, namebuf->length - 2, 0);
    if (res >= 0) {
        entry = e->data;
        ccn_replace_handler(h, &(entry->action), action);
        if (action == NULL)
            hashtb_delete(e);
    }
    hashtb_end(e);
    return(res);
}

static int
ccn_pushout(struct ccn *h)
{
    ssize_t res;
    size_t size;
    if (h->outbuf != NULL && h->outbufindex < h->outbuf->length) {
        if (h->sock < 0)
            return(1);
        size = h->outbuf->length - h->outbufindex;
        res = write(h->sock, h->outbuf->buf + h->outbufindex, size);
        if (res == size) {
            h->outbuf->length = h->outbufindex = 0;
            return(0);
        }
        if (res == -1)
            return ((errno == EAGAIN) ? 1 : NOTE_ERRNO(h));
        h->outbufindex += res;
        return(1);
    }
    return(0);
}

int
ccn_put(struct ccn *h, const void *p, size_t length)
{
    struct ccn_skeleton_decoder dd = {0};
    ssize_t res;
    if (h == NULL || p == NULL || length == 0)
        return(NOTE_ERR(h, EINVAL));
    res = ccn_skeleton_decode(&dd, p, length);
    if (!(res == length && dd.state == 0))
        return(NOTE_ERR(h, EINVAL));
    if (h->tap != -1) {
	res = write(h->tap, p, length);
        if (res == -1) {
            NOTE_ERRNO(h);
            (void)close(h->tap);
            h->tap = -1;
        }
    }
    if (h->outbuf != NULL && h->outbufindex < h->outbuf->length) {
        // XXX - should limit unbounded growth of h->outbuf
        ccn_charbuf_append(h->outbuf, p, length); // XXX - check res
        return (ccn_pushout(h));
    }
    if (h->sock == -1)
        res = 0;
    else
        res = write(h->sock, p, length);
    if (res == length)
        return(0);
    if (res == -1) {
        if (errno != EAGAIN)
            return(NOTE_ERRNO(h));
        res = 0;
    }
    if (h->outbuf == NULL) {
        h->outbuf = ccn_charbuf_create();
        h->outbufindex = 0;
    }
    ccn_charbuf_append(h->outbuf, ((const unsigned char *)p)+res, length-res);
    return(1);
}

int
ccn_output_is_pending(struct ccn *h)
{
    return(h != NULL && h->outbuf != NULL && h->outbufindex < h->outbuf->length);
}

struct ccn_charbuf *
ccn_grab_buffered_output(struct ccn *h)
{
    if (ccn_output_is_pending(h) && h->outbufindex == 0) {
        struct ccn_charbuf *ans = h->outbuf;
        h->outbuf = NULL;
        return(ans);
    }
    return(NULL);
}

static void
ccn_refresh_interest(struct ccn *h, struct expressed_interest *interest)
{
    int res;
    if (interest->magic != 0x7059e5f4) {
        ccn_gripe(interest);
        return;
    }
    if (interest->outstanding < interest->target) {
        res = ccn_put(h, interest->interest_msg, interest->size);
        if (res >= 0) {
            interest->outstanding += 1;
            if (h->now.tv_sec == 0)
                gettimeofday(&h->now, NULL);
            interest->lasttime = h->now;
        }
    }
}

static int
ccn_get_content_type(const unsigned char *ccnb,
                     const struct ccn_parsed_ContentObject *pco)
{
    enum ccn_content_type type = pco->type;
    (void)ccnb; // XXX - don't need now
    switch (type) {
        case CCN_CONTENT_DATA:
        case CCN_CONTENT_ENCR:
        case CCN_CONTENT_GONE:
        case CCN_CONTENT_KEY:
        case CCN_CONTENT_LINK:
        case CCN_CONTENT_NACK:
            return (type);
        default:
            return (-1);
    }
}

static int
ccn_cache_key(struct ccn *h,
              const unsigned char *ccnb, size_t size,
              struct ccn_parsed_ContentObject *pco)
{
    int type;
    struct ccn_pkey **entry;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;

    type = ccn_get_content_type(ccnb, pco);
    if (type != CCN_CONTENT_KEY) {
        return (0);
    }

    ccn_digest_ContentObject(ccnb, pco);
    if (pco->digest_bytes != sizeof(pco->digest)) {
        return(NOTE_ERR(h, EINVAL));
    }

    hashtb_start(h->keys, e);
    res = hashtb_seek(e, (void *)pco->digest, pco->digest_bytes, 0);
    if (res < 0) {
        hashtb_end(e);
        return(NOTE_ERRNO(h));
    }
    entry = e->data;
    if (res == HT_NEW_ENTRY) {
        struct ccn_pkey *pkey;
        const unsigned char *data = NULL;
        size_t data_size = 0;

        res = ccn_content_get_value(ccnb, size, pco, &data, &data_size);
        if (res < 0) {
            hashtb_delete(e);
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        pkey = ccn_d2i_pubkey(data, data_size);
        if (pkey == NULL) {
            hashtb_delete(e);
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        *entry = pkey;
    }
    hashtb_end(e);
    return (0);

}

/*
 * Examine a ContentObject and try to find the public key needed to
 * verify it.  It might be present in our cache of keys, or in the
 * object itself; in either of these cases, we can satisfy the request
 * right away. Or there may be an indirection (a KeyName), in which case
 * return without the key. The final possibility
 * is that there is no key locator we can make sense of.
 * Returns negative for error, 0 when pubkey is filled in,
 *         or 1 if the key needs to be requested.
 */
static int
ccn_locate_key(struct ccn *h,
               unsigned char *msg,
               size_t size,
               struct ccn_parsed_ContentObject *pco,
               struct ccn_pkey **pubkey)
{
    int res;
    const unsigned char *pkeyid;
    size_t pkeyid_size;
    struct ccn_pkey **entry;
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;

    if (h->keys == NULL) {
        return (NOTE_ERR(h, EINVAL));
    }

    res = ccn_ref_tagged_BLOB(CCN_DTAG_PublisherPublicKeyDigest, msg,
                              pco->offset[CCN_PCO_B_PublisherPublicKeyDigest],
                              pco->offset[CCN_PCO_E_PublisherPublicKeyDigest],
                              &pkeyid, &pkeyid_size);
    if (res < 0)
        return (NOTE_ERR(h, res));
    entry = hashtb_lookup(h->keys, pkeyid, pkeyid_size);
    if (entry != NULL) {
        *pubkey = *entry;
        return (0);
    }
    /* Is a key locator present? */
    if (pco->offset[CCN_PCO_B_KeyLocator] == pco->offset[CCN_PCO_E_KeyLocator])
        return (-1);
    /* Use the key locator */
    d = ccn_buf_decoder_start(&decoder, msg + pco->offset[CCN_PCO_B_Key_Certificate_KeyName],
                              pco->offset[CCN_PCO_E_Key_Certificate_KeyName] -
                              pco->offset[CCN_PCO_B_Key_Certificate_KeyName]);
    if (ccn_buf_match_dtag(d, CCN_DTAG_KeyName)) {
        return(1);
    }
    else if (ccn_buf_match_dtag(d, CCN_DTAG_Key)) {
        const unsigned char *dkey;
        size_t dkey_size;
        struct ccn_digest *digest = NULL;
        unsigned char *key_digest = NULL;
        size_t key_digest_size;
        struct hashtb_enumerator ee;
        struct hashtb_enumerator *e = &ee;

        res = ccn_ref_tagged_BLOB(CCN_DTAG_Key, msg,
                                  pco->offset[CCN_PCO_B_Key_Certificate_KeyName],
                                  pco->offset[CCN_PCO_E_Key_Certificate_KeyName],
                                  &dkey, &dkey_size);
        *pubkey = ccn_d2i_pubkey(dkey, dkey_size);
        digest = ccn_digest_create(CCN_DIGEST_SHA256);
        ccn_digest_init(digest);
        key_digest_size = ccn_digest_size(digest);
        key_digest = calloc(1, key_digest_size);
        if (key_digest == NULL) abort();
        res = ccn_digest_update(digest, dkey, dkey_size);
        if (res < 0) abort();
        res = ccn_digest_final(digest, key_digest, key_digest_size);
        if (res < 0) abort();
        ccn_digest_destroy(&digest);
        hashtb_start(h->keys, e);
        res = hashtb_seek(e, (void *)key_digest, key_digest_size, 0);
        free(key_digest);
        key_digest = NULL;
        if (res < 0) {
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        entry = e->data;
        if (res == HT_NEW_ENTRY) {
            *entry = *pubkey;
        }
        else
            THIS_CANNOT_HAPPEN(h);
        hashtb_end(e);
        return (0);
    }
    else if (ccn_buf_match_dtag(d, CCN_DTAG_Certificate)) {
        XXX; // what should we really do in this case?
    }

    return (-1);
}

/*
 * Called when we get an answer to a KeyLocator fetch issued by
 * ccn_initiate_key_fetch.  This does not really have to do much,
 * since the main content handling logic picks up the keys as they
 * go by.
 */
static enum ccn_upcall_res
handle_key(
           struct ccn_closure *selfp,
           enum ccn_upcall_kind kind,
           struct ccn_upcall_info *info)
{
    struct ccn *h = info->h;
    (void)h;
    switch(kind) {
        case CCN_UPCALL_FINAL:
            free(selfp);
            return(CCN_UPCALL_RESULT_OK);
        case CCN_UPCALL_INTEREST_TIMED_OUT:
            /* Don't keep trying */
            return(CCN_UPCALL_RESULT_OK);
        case CCN_UPCALL_CONTENT:
        case CCN_UPCALL_CONTENT_UNVERIFIED:
            return(CCN_UPCALL_RESULT_OK);
        default:
            return (CCN_UPCALL_RESULT_ERR);
    }
}

static int
ccn_initiate_key_fetch(struct ccn *h,
                       unsigned char *msg,
                       struct ccn_parsed_ContentObject *pco,
                       struct expressed_interest *trigger_interest)
{
    /* 
     * Create a new interest in the key name, set up a callback that will
     * insert the key into the h->keys hashtb for the calling handle and
     * cause the trigger_interest to be re-expressed.
     */
    int res;
    int namelen;
    struct ccn_charbuf *key_name = NULL;
    struct ccn_closure *key_closure = NULL;
    const unsigned char *pkeyid = NULL;
    size_t pkeyid_size = 0;
    struct ccn_charbuf *templ = NULL;
    
    if (trigger_interest != NULL) {
        /* Arrange a wakeup when the key arrives */
        if (trigger_interest->wanted_pub == NULL)
            trigger_interest->wanted_pub = ccn_charbuf_create();
        res = ccn_ref_tagged_BLOB(CCN_DTAG_PublisherPublicKeyDigest, msg,
                                  pco->offset[CCN_PCO_B_PublisherPublicKeyDigest],
                                  pco->offset[CCN_PCO_E_PublisherPublicKeyDigest],
                                  &pkeyid, &pkeyid_size);
        if (trigger_interest->wanted_pub != NULL && res >= 0) {
            trigger_interest->wanted_pub->length = 0;
            ccn_charbuf_append(trigger_interest->wanted_pub, pkeyid, pkeyid_size);
        }
        trigger_interest->target = 0;
    }

    namelen = (pco->offset[CCN_PCO_E_KeyName_Name] -
               pco->offset[CCN_PCO_B_KeyName_Name]);
    /*
     * If there is no KeyName provided, we can't ask, but we might win if the
     * key arrives along with some other content.
     */
    if (namelen == 0)
        return(-1);
    key_closure = calloc(1, sizeof(*key_closure));
    if (key_closure == NULL)
        return (NOTE_ERRNO(h));
    key_closure->p = &handle_key;
    
    key_name = ccn_charbuf_create();
    res = ccn_charbuf_append(key_name,
                             msg + pco->offset[CCN_PCO_B_KeyName_Name],
                             namelen);
    if (pco->offset[CCN_PCO_B_KeyName_Pub] < pco->offset[CCN_PCO_E_KeyName_Pub]) {
        templ = ccn_charbuf_create();
        ccn_charbuf_append_tt(templ, CCN_DTAG_Interest, CCN_DTAG);
        ccn_charbuf_append_tt(templ, CCN_DTAG_Name, CCN_DTAG);
        ccn_charbuf_append_closer(templ); /* </Name> */
        ccn_charbuf_append(templ,
                           msg + pco->offset[CCN_PCO_B_KeyName_Pub],
                           (pco->offset[CCN_PCO_E_KeyName_Pub] - 
                            pco->offset[CCN_PCO_B_KeyName_Pub]));
        ccn_charbuf_append_closer(templ); /* </Interest> */
    }
    res = ccn_express_interest(h, key_name, -1, key_closure, templ);
    ccn_charbuf_destroy(&key_name);
    ccn_charbuf_destroy(&templ);
    return(res);
}

/*
 * If we were waiting for a key and it has arrived,
 * refresh the interest.
 */
static void
ccn_check_pub_arrival(struct ccn *h, struct expressed_interest *interest)
{
    struct ccn_charbuf *want = interest->wanted_pub;
    if (want == NULL)
        return;
    if (hashtb_lookup(h->keys, want->buf, want->length) != NULL) {
        ccn_charbuf_destroy(&interest->wanted_pub);
        interest->target = 1;
        ccn_refresh_interest(h, interest);
    }
}

/**
 * Dispatch a message through the registered upcalls.
 * This is not used by normal ccn clients, but is made available for use when
 * ccnd needs to communicate with its internal client.
 * @param h is the ccn handle.
 * @param msg is the ccnb-encoded Interest or ContentObject.
 * @param size is its size in bytes.
 */
void
ccn_dispatch_message(struct ccn *h, unsigned char *msg, size_t size)
{
    struct ccn_parsed_interest pi = {0};
    struct ccn_upcall_info info = {0};
    int i;
    int res;
    enum ccn_upcall_res ures;
    
    h->running++;
    info.h = h;
    info.pi = &pi;
    info.interest_comps = ccn_indexbuf_obtain(h);
    res = ccn_parse_interest(msg, size, &pi, info.interest_comps);
    if (res >= 0) {
        /* This message is an Interest */
        enum ccn_upcall_kind upcall_kind = CCN_UPCALL_INTEREST;
        info.interest_ccnb = msg;
        if (h->interest_filters != NULL && info.interest_comps->n > 0) {
            struct ccn_indexbuf *comps = info.interest_comps;
            size_t keystart = comps->buf[0];
            unsigned char *key = msg + keystart;
            struct interest_filter *entry;
            for (i = comps->n - 1; i >= 0; i--) {
                entry = hashtb_lookup(h->interest_filters, key, comps->buf[i] - keystart);
                if (entry != NULL) {
                    info.matched_comps = i;
                    ures = (entry->action->p)(entry->action, upcall_kind, &info);
                    if (ures == CCN_UPCALL_RESULT_INTEREST_CONSUMED)
                        upcall_kind = CCN_UPCALL_CONSUMED_INTEREST;
                }
            }
        }
    }
    else {
        /* This message should be a ContentObject. */
        struct ccn_parsed_ContentObject obj = {0};
        info.pco = &obj;
        info.content_comps = ccn_indexbuf_create();
        res = ccn_parse_ContentObject(msg, size, &obj, info.content_comps);
        if (res >= 0) {
            info.content_ccnb = msg;
            if (h->interests_by_prefix != NULL) {
                struct ccn_indexbuf *comps = info.content_comps;
                size_t keystart = comps->buf[0];
                unsigned char *key = msg + keystart;
                struct expressed_interest *interest = NULL;
                struct interests_by_prefix *entry = NULL;
                for (i = comps->n - 1; i >= 0; i--) {
                    entry = hashtb_lookup(h->interests_by_prefix, key, comps->buf[i] - keystart);
                    if (entry != NULL) {
                        for (interest = entry->list; interest != NULL; interest = interest->next) {
                            if (interest->magic != 0x7059e5f4) {
                                ccn_gripe(interest);
                            }
                            if (interest->target > 0 && interest->outstanding > 0) {
                                res = ccn_parse_interest(interest->interest_msg,
                                                         interest->size,
                                                         info.pi,
                                                         info.interest_comps);
                                if (res >= 0 &&
                                    ccn_content_matches_interest(msg, size,
                                                                 1, info.pco,
                                                                 interest->interest_msg,
                                                                 interest->size,
                                                                 info.pi)) {
                                    enum ccn_upcall_kind upcall_kind; /* KEYS */
                                    struct ccn_pkey *pubkey = NULL;
                                    int type = ccn_get_content_type(msg, info.pco);
                                    if (type == CCN_CONTENT_KEY) {
                                        res = ccn_cache_key(h, msg, size, info.pco);
                                    }
                                    res = ccn_locate_key(h, msg, size, info.pco, &pubkey);
                                    if (res == 0) {
                                        /* we have the pubkey, use it to verify the msg */
                                        res = ccn_verify_signature(msg, size, info.pco, pubkey);
                                        upcall_kind = (res == 1) ? CCN_UPCALL_CONTENT : CCN_UPCALL_CONTENT_BAD;
                                    } else {
                                        upcall_kind = CCN_UPCALL_CONTENT_UNVERIFIED;
                                    }
                                    interest->outstanding -= 1;
                                    info.interest_ccnb = interest->interest_msg;
                                    info.matched_comps = i;
                                    ures = (interest->action->p)(interest->action,
                                                                 upcall_kind,
                                                                 &info);
                                    if (interest->magic != 0x7059e5f4)
                                        ccn_gripe(interest);
                                    if (ures == CCN_UPCALL_RESULT_REEXPRESS)
                                        ccn_refresh_interest(h, interest);
                                    else if (ures == CCN_UPCALL_RESULT_VERIFY &&
                                             upcall_kind == CCN_UPCALL_CONTENT_UNVERIFIED) { /* KEYS */
                                        ccn_initiate_key_fetch(h, msg, info.pco, interest);
                                    } else {
                                        interest->target = 0;
                                        replace_interest_msg(interest, NULL);
                                        ccn_replace_handler(h, &(interest->action), NULL);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } // XXX whew, what a lot of right braces!
    ccn_indexbuf_release(h, info.interest_comps);
    ccn_indexbuf_destroy(&info.content_comps);
    h->running--;
}

static int
ccn_process_input(struct ccn *h)
{
    ssize_t res;
    ssize_t msgstart;
    unsigned char *buf;
    struct ccn_skeleton_decoder *d = &h->decoder;
    struct ccn_charbuf *inbuf = h->inbuf;
    if (inbuf == NULL)
        h->inbuf = inbuf = ccn_charbuf_create();
    if (inbuf->length == 0)
        memset(d, 0, sizeof(*d));
    buf = ccn_charbuf_reserve(inbuf, 8800);
    res = read(h->sock, buf, inbuf->limit - inbuf->length);
    if (res == 0) {
        ccn_disconnect(h);
        return(-1);
    }
    if (res == -1) {
        if (errno == EAGAIN)
            res = 0;
        else
            return(NOTE_ERRNO(h));
    }
    inbuf->length += res;
    msgstart = 0;
    ccn_skeleton_decode(d, buf, res);
    while (d->state == 0) {
        ccn_dispatch_message(h, inbuf->buf + msgstart, 
                              d->index - msgstart);
        msgstart = d->index;
        if (msgstart == inbuf->length) {
            inbuf->length = 0;
            return(0);
        }
        ccn_skeleton_decode(d, inbuf->buf + d->index,
                            inbuf->length - d->index);
    }
    if (msgstart < inbuf->length && msgstart > 0) {
        /* move partial message to start of buffer */
        memmove(inbuf->buf, inbuf->buf + msgstart,
                inbuf->length - msgstart);
        inbuf->length -= msgstart;
        d->index -= msgstart;
    }
    return(0);
}

static void
ccn_age_interest(struct ccn *h,
                 struct expressed_interest *interest,
                 const unsigned char *key, size_t keysize)
{
    struct ccn_parsed_interest pi = {0};
    struct ccn_upcall_info info = {0};
    int delta;
    int res;
    enum ccn_upcall_res ures;
    int firstcall;
    if (interest->magic != 0x7059e5f4)
        ccn_gripe(interest);
    info.h = h;
    info.pi = &pi;
    firstcall = (interest->lasttime.tv_sec == 0);
    if (interest->lasttime.tv_sec + 30 < h->now.tv_sec) {
        /* fixup so that delta does not overflow */
        interest->outstanding = 0;
        interest->lasttime = h->now;
        interest->lasttime.tv_sec -= 30;
    }
    delta = (h->now.tv_sec  - interest->lasttime.tv_sec)*1000000 +
            (h->now.tv_usec - interest->lasttime.tv_usec);
    if (delta >= CCN_INTEREST_LIFETIME_MICROSEC) {
        interest->outstanding = 0;
        delta = 0;
    }
    else if (delta < 0)
        delta = 0;
    if (CCN_INTEREST_LIFETIME_MICROSEC - delta < h->refresh_us)
        h->refresh_us = CCN_INTEREST_LIFETIME_MICROSEC - delta;
    interest->lasttime = h->now;
    while (delta > interest->lasttime.tv_usec) {
        delta -= 1000000;
        interest->lasttime.tv_sec -= 1;
    }
    interest->lasttime.tv_usec -= delta;
    if (interest->target > 0 && interest->outstanding == 0) {
        ures = CCN_UPCALL_RESULT_REEXPRESS;
        if (!firstcall) {
            info.interest_ccnb = interest->interest_msg;
            info.interest_comps = ccn_indexbuf_obtain(h);
            res = ccn_parse_interest(interest->interest_msg,
                                     interest->size,
                                     info.pi,
                                     info.interest_comps);
            if (res >= 0) {
                ures = (interest->action->p)(interest->action,
                                             CCN_UPCALL_INTEREST_TIMED_OUT,
                                             &info);
                if (interest->magic != 0x7059e5f4)
                    ccn_gripe(interest);
            }
            else {
                int i;
                fprintf(stderr, "URP!! interest has been corrupted ccn_client.c:%d\n", __LINE__);
                for (i = 0; i < 120; i++)
                    sleep(1);
                ures = CCN_UPCALL_RESULT_ERR;
            }
            ccn_indexbuf_release(h, info.interest_comps);
        }
        if (ures == CCN_UPCALL_RESULT_REEXPRESS)
            ccn_refresh_interest(h, interest);
        else
            interest->target = 0;
    }
}

static void
ccn_clean_all_interests(struct ccn *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct interests_by_prefix *entry;
    for (hashtb_start(h->interests_by_prefix, e); e->data != NULL;) {
        entry = e->data;
        ccn_clean_interests_by_prefix(h, entry);
        if (entry->list == NULL)
            hashtb_delete(e);
        else
            hashtb_next(e);
    }
    hashtb_end(e);
}

/**
 * Process any scheduled operations that are due.
 * This is not used by normal ccn clients, but is made available for use
 * by ccnd to run its internal client.
 * @param h is the ccn handle.
 * @returns the number of microseconds until the next thing needs to happen.
 */
int
ccn_process_scheduled_operations(struct ccn *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct interests_by_prefix *entry;
    struct expressed_interest *ie;
    int need_clean = 0;
    h->refresh_us = 5 * CCN_INTEREST_LIFETIME_MICROSEC;
    gettimeofday(&h->now, NULL);
    if (ccn_output_is_pending(h))
        return(h->refresh_us);
    h->running++;
    if (h->interest_filters != NULL) {
        for (hashtb_start(h->interest_filters, e); e->data != NULL; hashtb_next(e)) {
            struct interest_filter *i = e->data;
            // XXX If the registration is expiring, refresh it
            // Otherwise update h->refresh_us
            if (i == NULL) abort(); // Silence unused var warning for now.
        }
        hashtb_end(e);
    }
    if (h->interests_by_prefix != NULL) {
        for (hashtb_start(h->interests_by_prefix, e); e->data != NULL; hashtb_next(e)) {
            entry = e->data;
            ccn_check_interests(entry->list);
            if (entry->list == NULL)
                need_clean = 1;
            else {
                for (ie = entry->list; ie != NULL; ie = ie->next) {
                    ccn_check_pub_arrival(h, ie);
                    if (ie->target != 0)
                        ccn_age_interest(h, ie, e->key, e->keysize);
                    if (ie->target == 0 && ie->wanted_pub == NULL) {
                        ccn_replace_handler(h, &(ie->action), NULL);
                        replace_interest_msg(ie, NULL);
                        need_clean = 1;
                    }
                }
            }
        }
        hashtb_end(e);
        if (need_clean)
            ccn_clean_all_interests(h);
    }
    h->running--;
    return(h->refresh_us);
}

/*
 * Modify ccn_run timeout.
 * This may be called from an upcall to change the timeout value.
 * Most often this will be used to set the timeout to zero so that
 * ccn_run will return control to the client.
 * @param h is the ccn handle.
 * @param timeout is in milliseconds.
 * @returns old timeout value.
 */
int
ccn_set_run_timeout(struct ccn *h, int timeout)
{
    int ans = h->timeout;
    h->timeout = timeout;
    return(ans);
}

/*
 * Run the ccn client event loop.
 * This may serve as the main event loop for simple apps by passing 
 * a timeout value of -1.
 * @param h is the ccn handle.
 * @param timeout is in milliseconds.
 * @returns a negative value for error, zero for success.
 */
int
ccn_run(struct ccn *h, int timeout)
{
    struct timeval start;
    struct pollfd fds[1];
    int microsec;
    int millisec;
    int res = -1;
    if (h->running != 0)
        return(NOTE_ERR(h, EBUSY));
    memset(fds, 0, sizeof(fds));
    memset(&start, 0, sizeof(start));
    h->timeout = timeout;
    for (;;) {
        if (h->sock == -1) {
            res = -1;
            break;
        }
        microsec = ccn_process_scheduled_operations(h);
        timeout = h->timeout;
        if (start.tv_sec == 0)
            start = h->now;
        else if (timeout >= 0) {
            millisec = (h->now.tv_sec  - start.tv_sec) *1000 +
            (h->now.tv_usec - start.tv_usec)/1000;
            if (millisec > timeout) {
                res = 0;
                break;
            }
        }
        fds[0].fd = h->sock;
        fds[0].events = POLLIN;
        if (ccn_output_is_pending(h))
            fds[0].events |= POLLOUT;
        millisec = microsec / 1000;
        if (timeout >= 0 && timeout < millisec)
            millisec = timeout;
        res = poll(fds, 1, millisec);
        if (res < 0 && errno != EINTR) {
            res = NOTE_ERRNO(h);
            break;
        }
        if (res > 0) {
            if ((fds[0].revents | POLLOUT) != 0)
                ccn_pushout(h);
            if ((fds[0].revents | POLLIN) != 0)
                ccn_process_input(h);
        }
        if (h->err == ENOTCONN)
            ccn_disconnect(h);
        if (h->timeout == 0)
            break;
    }
    if (h->running != 0)
        abort();
    return((res < 0) ? res : 0);
}

/* This is the upcall for implementing ccn_get() */
struct simple_get_data {
    struct ccn_closure closure;
    struct ccn_charbuf *resultbuf;
    struct ccn_parsed_ContentObject *pcobuf;
    struct ccn_indexbuf *compsbuf;
    int res;
};

static enum ccn_upcall_res
handle_simple_incoming_content(
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info)
{
    struct simple_get_data *md = selfp->data;
    struct ccn *h = info->h;
    
    if (kind == CCN_UPCALL_FINAL) {
        if (selfp != &md->closure)
            abort();
        free(md);
        return(CCN_UPCALL_RESULT_OK);
    }
    if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
        return(selfp->intdata ? CCN_UPCALL_RESULT_REEXPRESS : CCN_UPCALL_RESULT_OK);
    if (kind == CCN_UPCALL_CONTENT_UNVERIFIED)
        XXX; // - Probably should always work hard to verify, or add a parameter to specify.
    if (kind != CCN_UPCALL_CONTENT && kind != CCN_UPCALL_CONTENT_UNVERIFIED)
        return(CCN_UPCALL_RESULT_ERR);
    if (md->resultbuf != NULL) {
        md->resultbuf->length = 0;
        ccn_charbuf_append(md->resultbuf,
                           info->content_ccnb, info->pco->offset[CCN_PCO_E]);
    }
    if (md->pcobuf != NULL)
        memcpy(md->pcobuf, info->pco, sizeof(*md->pcobuf));
    if (md->compsbuf != NULL) {
        md->compsbuf->n = 0;
        ccn_indexbuf_append(md->compsbuf,
                            info->content_comps->buf, info->content_comps->n);
    }
    md->res = 0;
    ccn_set_run_timeout(info->h, 0);
    return(CCN_UPCALL_RESULT_OK);
}

/**
 * Get a single matching ContentObject
 * This is a convenience for getting a single matching ContentObject.
 * Blocks until a matching ContentObject arrives or there is a timeout.
 * @param h is the ccn handle. If NULL or ccn_get is called from inside
 *        an upcall, a new connection will be used and upcalls from other
 *        requests will not be processed while ccn_get is active.
 * @param name holds a ccnb-encoded Name
 * @param interest_template conveys other fields to be used in the interest
 *        (may be NULL).
 * @param timeout_ms limits the time spent waiting for an answer (milliseconds).
 * @param pcobuf may be supplied to save the client the work of re-parsing the
 *        ContentObject; may be NULL if this information is not actually needed.
 * @param compsbuf works similarly.
 * @param resultbuf is updated to contain the ccnb-encoded ContentObject.
 * @returns 0 for success, -1 for an error.
 */
int
ccn_get(struct ccn *h,
        struct ccn_charbuf *name,
        int prefix_comps,
        struct ccn_charbuf *interest_template,
        int timeout_ms,
        struct ccn_charbuf *resultbuf,
        struct ccn_parsed_ContentObject *pcobuf,
        struct ccn_indexbuf *compsbuf)
{
    struct ccn *orig_h = h;
    struct hashtb *saved_keys = NULL;
    int res;
    struct simple_get_data *md;
    
    if (h == NULL || h->running) {
        h = ccn_create();
        if (h == NULL)
            return(-1);
        if (orig_h != NULL) { /* Dad, can I borrow the keys? */
            saved_keys = h->keys;
            h->keys = orig_h->keys;
        }
        res = ccn_connect(h, NULL);
        if (res < 0) {
            ccn_destroy(&h);
            return(-1);
        }
    }
    md = calloc(1, sizeof(*md));
    md->resultbuf = resultbuf;
    md->pcobuf = pcobuf;
    md->compsbuf = compsbuf;
    md->res = -1;
    md->closure.p = &handle_simple_incoming_content;
    md->closure.data = md;
    md->closure.intdata = 1; /* tell upcall to re-express if needed */
    md->closure.refcount = 1;
    res = ccn_express_interest(h, name, prefix_comps, &md->closure, interest_template);
    if (res >= 0)
        res = ccn_run(h, timeout_ms);
    if (res >= 0)
        res = md->res;
    md->resultbuf = NULL;
    md->pcobuf = NULL;
    md->compsbuf = NULL;
    md->closure.intdata = 0;
    md->closure.refcount--;
    if (md->closure.refcount == 0)
        free(md);
    if (h != orig_h) {
        if (saved_keys != NULL)
            h->keys = saved_keys;
        ccn_destroy(&h);
    }
    return(res);
}