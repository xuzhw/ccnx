/*
 * Simple program for smoke-test of ccn client lib
 * Author: Michael Plass
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ccn/ccn.h>
#include <unistd.h>

void printraw(const void *r, int n)
{
    int i, l;
    const unsigned char *p = r;
    while (n > 0) {
        l = (n > 40 ? 40 : n);
        for (i = 0; i < l; i++)
            printf(" %c", (' ' <= p[i] && p[i] <= '~') ? p[i] : '.');
        printf("\n");
        for (i = 0; i < l; i++)
            printf("%02X", p[i]);
        printf("\n");
        p += l;
        n -= l;
    }
}

int
incoming_content(
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn *h,
    const unsigned char *ccnb,    /* binary-format Interest or ContentObject */
    size_t ccnb_size,             /* size in bytes */
    struct ccn_indexbuf *comps,   /* component boundaries within ccnb */
    int matched_comps,            /* number of components in registration */
    const unsigned char *matched_ccnb, /* binary-format matched Interest */
    size_t matched_ccnb_size
)
{
    if (kind == CCN_UPCALL_FINAL)
        return(0);
    if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
        return(CCN_UPCALL_RESULT_REEXPRESS);
    if (kind != CCN_UPCALL_CONTENT)
        return(-1);
    printf("Got content matching %d components:\n", matched_comps);
    printraw(ccnb, ccnb_size);
    return(0);
}

/* Use some static data for this simple program */
static struct ccn_closure incoming_content_action = {
    .p = &incoming_content
};

static unsigned char rawbuf[1024*1024];
static ssize_t rawlen;

int
outgoing_content(
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn *h,
    const unsigned char *ccnb,    /* binary-format Interest or ContentObject */
    size_t ccnb_size,             /* size in bytes */
    struct ccn_indexbuf *comps,   /* component boundaries within ccnb */
    int matched_comps,            /* number of components in registration */
    const unsigned char *matched_ccnb, /* binary-format matched Interest */
    size_t matched_ccnb_size
)
{
    int res = 0;
    if (kind == CCN_UPCALL_FINAL) {
        printf("CCN_UPCALL_FINAL for outgoing_content()\n");
        return(res);
    }
    printf("Got interest matching %d components, kind = %d\n", matched_comps, kind);
    if (kind == CCN_UPCALL_INTEREST) {
        res = ccn_put(h, rawbuf, rawlen);
        if (res == -1) {
            fprintf(stderr, "error sending data");
            return(-1);
        }
        else {
            printf("Sent my content:\n");
            printraw(rawbuf, rawlen);
            return(0);
        }
    }
    else
        return(-1);
}

static struct ccn_closure interest_filter = {
    .p = &outgoing_content
};

int
main(int argc, char **argv)
{
    int ch;
    int res;
    char *filename = NULL;
    int rep = 1;
    struct ccn *ccn = NULL;
    struct ccn_parsed_interest interest = {0};
    int i;
    struct ccn_charbuf *c = ccn_charbuf_create();
    struct ccn_charbuf *templ = ccn_charbuf_create();
    struct ccn_indexbuf *comps = ccn_indexbuf_create();
    while ((ch = getopt(argc, argv, "hf:n:")) != -1) {
        switch (ch) {
            default:
            case 'h':
                fprintf(stderr, "provide names of files containing ccnb format interests and content\n");
                exit(1);
	    case 'n':
		rep = atoi(optarg);
		break;
        }
    }
    argc -= optind;
    argv += optind;
    ccn = ccn_create();
    if (ccn_connect(ccn, NULL) == -1) {
        perror("ccn_connect");
        exit(1);
    }
    for (i = 0; argv[i] != NULL; i++) {
        filename = argv[i];
        close(0);
        res = open(filename, O_RDONLY);
        if (res != 0) {
            perror(filename);
            exit(1);
        }
        fprintf(stderr, "Reading %s ... ", filename);
        rawlen = read(0, rawbuf, sizeof(rawbuf));
        if (rawlen < 0) {
            perror("skipping");
            continue;
        }
        res = ccn_parse_interest(rawbuf, rawlen, &interest, NULL);
        if (res >= 0) {
            size_t name_start = interest.offset[CCN_PI_B_Name];
            size_t name_size = interest.offset[CCN_PI_E_Name] - name_start;
            templ->length = 0;
            ccn_charbuf_append(templ, rawbuf, rawlen);
            fprintf(stderr, "Registering interest with %d name components\n", res);
            c->length = 0;
            ccn_charbuf_append(c, rawbuf + name_start, name_size);
            // XXX - rep is currently ignored
            ccn_express_interest(ccn, c, interest.prefix_comps, &incoming_content_action, templ);
        }
        else {
            struct ccn_parsed_ContentObject obj = {0};
            int k;
            res = ccn_parse_ContentObject(rawbuf, rawlen, &obj, comps);
            if (res >= 0) {
                fprintf(stderr, "Offering content\n");
                /* We won't listen for interests with fewer than 2 name component */
                for (k = comps->n - 1; k >= 2; k--) {
                    c->length = 0;
                    ccn_charbuf_append_tt(c, CCN_DTAG_Name, CCN_DTAG);
                    ccn_charbuf_append(c, rawbuf+comps->buf[0], comps->buf[k] - comps->buf[0]);
                    ccn_charbuf_append_closer(c);
                    res = ccn_set_interest_filter(ccn, c, &interest_filter);
                    if (res < 0) abort();
                }
                res = ccn_run(ccn, 1000);
                /* Stop listening for these interests now */
                for (k = comps->n - 1; k >= 2; k--) {
                    c->length = 0;
                    ccn_charbuf_append_tt(c, CCN_DTAG_Name, CCN_DTAG);
                    ccn_charbuf_append(c, rawbuf+comps->buf[0], comps->buf[k] - comps->buf[0]);
                    ccn_charbuf_append_closer(c);
                    res = ccn_set_interest_filter(ccn, c, NULL);
                    if (res < 0) abort();
                }
            }
            else {
                fprintf(stderr, "what's that?\n");
            }
        }
    }
    fprintf(stderr, "Running for 8 more seconds\n");
    res = ccn_run(ccn, 8000);
    ccn_destroy(&ccn);
    exit(0);
}
