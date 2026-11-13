// How Biblionaut has expat built. expat's sources include this in place of
// the file its own build would write.
//
// The settings are for reading EPUBs on a device with little memory:
// everything that an EPUB's XML does not need is left out.
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

// Both the desktops this is built on and the device keep the low byte first.
#define BYTEORDER 1234

// General entities are not expanded: an entity a document declares for
// itself comes out as the reference it was written as, and the entities of
// HTML, such as &nbsp;, are reported by name. The five every XML document
// has (&amp; and the rest) still work. With no expansion there is nothing
// for a document to expand without end.
#define XML_GE 0

// No bytes of the document kept around for showing where an error was.
// Line and offset are still to be had.
#define XML_CONTEXT_BYTES 0

// XML_DTD (parameter entities and external subsets) and XML_NS (namespace
// processing) are left undefined: neither is used.

// expat wants a source of random numbers for the salt of its hash tables,
// and refuses to compile without one or this. It is given this, and never
// uses it: the salt is always set by whoever creates the parser (epub/xml.h).
#define XML_POOR_ENTROPY 1

#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1

#endif  // EXPAT_CONFIG_H
