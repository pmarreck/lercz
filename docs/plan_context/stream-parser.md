# Stream parser

The blob decoder stays the oracle and keeps its current API. A separate
forward-only parser feeds validate, through whichever container calls it.
It must not call `Lerc2::Decode`.

## Caller contract

The caller supplies a pull reader. The parser never looks back into a retained
blob. One unread byte of lookahead is allowed only to tell the end of a band
from the next byte.

The call returns a report when the walk finishes:

- each finding has an absolute byte offset, a stable code, and a category
- categories are format, codec, limit, io, and unsupported
- unsupported is an unimplemented payload mode, not a corrupt file
- `length` is set when the span is known (truncated tail, trailing bytes)
- a short caller array keeps the first findings and counts the rest as omitted
- `bytes_consumed` is how far the reader advanced

Samples are optional. With discard set, payload bytes are still parsed and
then dropped. Without it, a band is copied to the caller only after that
band's checks pass, and only while every earlier band was published. A failed
band leaves the caller's buffer unchanged from that band onward.

A band whose declared `blobSize` still delimits it is drained to that end so
the running Fletcher32 checksum can be compared, then the next band is parsed.
The walk stops when the next band boundary is no longer defined, or the reader
faults. Fletcher32 keeps the encoder's byte pairing and its reduction every
359 words. The version field stays outside the checksum, matching the format.

Lerc1 is not part of this API. The format does not change.

## First slice

Parsed now: Lerc2 headers for versions 2 through 6, the checksum, an empty
mask, a stored RLE mask, a constant band, per-depth constant ranges, one
raw one-sweep payload, and tiled bit-stuffing. The mask is MSB-first bits,
expanded with the byte RLE whose terminator is -32768. A later band may
omit the mask and reuse the previous band's bits, which is what the encoder
writes. Tile bits are unpacked LSB-first inside each little-endian uint32,
matching Lerc2 version 3 and later. A Huffman mode of 1, 2, or 3 is still
reported unsupported at that byte. Mode 0 falls through to tiles. Those
Huffman modes are still decoded by the oracle.
