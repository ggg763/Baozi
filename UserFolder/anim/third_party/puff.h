/* puff.h - minimal interface header for puff.c (inflate) in this project.
 *
 * The upstream puff.h is not shipped with puff.c, and this project only needs
 * the puff() prototype plus the NIL constant used for "scanning" mode.
 *
 * puff.c - deflate decompressor by Mark Adler (zlib license), version 2.3.
 */
#ifndef PUFF_H
#define PUFF_H

#define NIL 0                   /* null pointer for source/destination */
#define LOCAL static

/*
 * Decompress source[0..*sourcelen-1] into dest[0..*destlen-1].
 * Returns 0 on success, 1 = output space exhausted, 2 = ran out of input,
 * negative = corrupt deflate data.  On success, destlen and sourcelen are set to
 * the actual sizes; on positive errors they are left unchanged.
 */
int puff(unsigned char *dest,
         unsigned long *destlen,
         const unsigned char *source,
         unsigned long *sourcelen);

#endif /* PUFF_H */
