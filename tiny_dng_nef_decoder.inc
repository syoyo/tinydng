// NEF decoding code fragment is based on dcraw.

/*
   dcraw.c -- Dave Coffin's raw photo decoder
   Copyright 1997-2016 by Dave Coffin, dcoffin a cybercom o net

   This is a command-line ANSI C program to convert raw photos from
   any digital camera on any computer running any operating system.

   No license is required to download and use dcraw.c.  However,
   to lawfully redistribute dcraw, you must either (a) offer, at
   no extra charge, full source code* for all executable files
   containing RESTRICTED functions, (b) distribute this code under
   the GPL Version 2 or later, (c) remove all RESTRICTED functions,
   re-implement them, or copy them from an earlier, unrestricted
   Revision of dcraw.c, or (d) purchase a license from the author.

   The functions that process Foveon images have been RESTRICTED
   since Revision 1.237.  All other code remains free for all uses.

   *If you have not modified dcraw.c in any way, a link to my
   homepage qualifies as "full source code".

   $Revision: 1.477 $
   $Date: 2016/05/10 21:30:43 $
 */

// from dcraw

static const unsigned char nikon_tree[][32] = {
  { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy */
    5,4,3,6,2,7,1,0,8,9,11,10,12 },
  { 0,1,5,1,1,1,1,1,1,2,0,0,0,0,0,0,  /* 12-bit lossy after split */
    0x39,0x5a,0x38,0x27,0x16,5,4,3,2,1,0,11,12,12 },
  { 0,1,4,2,3,1,2,0,0,0,0,0,0,0,0,0,  /* 12-bit lossless */
    5,4,6,3,7,2,8,1,9,0,10,11,12 },
  { 0,1,4,3,1,1,1,1,1,2,0,0,0,0,0,0,  /* 14-bit lossy */
    5,6,4,7,8,3,9,2,1,0,10,11,12,13,14 },
  { 0,1,5,1,1,1,1,1,1,1,2,0,0,0,0,0,  /* 14-bit lossy after split */
    8,0x5c,0x4b,0x3a,0x29,7,6,5,4,3,2,1,0,13,14 },
  { 0,1,4,2,2,3,1,2,0,0,0,0,0,0,0,0,  /* 14-bit lossless */
    7,6,8,5,9,4,10,3,11,12,2,0,1,13,14 } };

