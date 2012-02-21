#ifndef __PORTAB_H__
#define __PORTAB_H__

#define IF_ELSE(a,b,c) ((a) ? (b) : (c))

#if defined(__G56K__)

#define S16BIT int
#define U16BIT unsigned int
#define S24BIT int
#define U24BIT unsigned int
#define S32BIT long
#define U32BIT unsigned long

#elif defined(__G21__) || defined(_TMS320C50)

#define S16BIT int
#define U16BIT unsigned int
#define S24BIT long
#define U24BIT unsigned long
#define S32BIT long
#define U32BIT unsigned long

#else

/*
#define S16BIT short
#define U16BIT unsigned short
#define S24BIT int
#define U24BIT unsigned int
#define S32BIT int
#define U32BIT unsigned int
*/
#include "mocad.H"

#endif
#endif


