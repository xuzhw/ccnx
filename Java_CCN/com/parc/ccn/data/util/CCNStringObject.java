/**
 * 
 */
package com.parc.ccn.data.util;

import java.io.IOException;

import javax.xml.stream.XMLStreamException;

import com.parc.ccn.config.ConfigurationException;
import com.parc.ccn.data.ContentName;
import com.parc.ccn.data.ContentObject;
import com.parc.ccn.data.security.PublisherPublicKeyDigest;
import com.parc.ccn.library.CCNLibrary;

public class CCNStringObject extends CCNSerializableObject<String> {

	public CCNStringObject(ContentName name, String data, CCNLibrary library) throws ConfigurationException, IOException {
		super(String.class, name, data, library);
	}
	
	public CCNStringObject(ContentName name, PublisherPublicKeyDigest publisher,
			CCNLibrary library) throws ConfigurationException, IOException, XMLStreamException {
		super(String.class, name, publisher, library);
	}
	
	/**
	 * Read constructor -- opens existing object.
	 * @param type
	 * @param name
	 * @param library
	 * @throws XMLStreamException
	 * @throws IOException
	 * @throws ClassNotFoundException 
	 */
	public CCNStringObject(ContentName name, 
			CCNLibrary library) throws ConfigurationException, IOException, XMLStreamException {
		super(String.class, name, (PublisherPublicKeyDigest)null, library);
	}
	
	public CCNStringObject(ContentObject firstBlock,
			CCNLibrary library) throws ConfigurationException, IOException, XMLStreamException {
		super(String.class, firstBlock, library);
	}
	
	public String string() { return data(); }
}