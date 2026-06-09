#ifndef TYPES_H
#define TYPES_H

typedef unsigned char			u8;	/* unsigned  8-bit */
typedef unsigned short			u16;	/* unsigned 16-bit */
typedef unsigned int			u32;	/* unsigned 32-bit */
typedef unsigned long long		u64;	/* unsigned 64-bit */

typedef signed char			s8;	/* signed  8-bit */
typedef short				s16;	/* signed 16-bit */
typedef int				s32;	/* signed 32-bit */
typedef long long			s64;	/* signed 64-bit */

typedef volatile unsigned char		vu8;	/* unsigned  8-bit */
typedef volatile unsigned short		vu16;	/* unsigned 16-bit */
typedef volatile unsigned int		vu32;	/* unsigned 32-bit */
typedef volatile unsigned long long	vu64;	/* unsigned 64-bit */

typedef volatile signed char	vs8;	/* signed  8-bit */
typedef volatile short			vs16;	/* signed 16-bit */
typedef volatile int			vs32;	/* signed 32-bit */
typedef volatile long long		vs64;	/* signed 64-bit */

typedef float				f32;	/* single prec floating point */
typedef double				f64;	/* double prec floating point */

// Endian-aware accessor macros for RDRAM reads/writes.
// GLideN64 stores RDRAM in host byte order. On LE hosts, 16-bit and 8-bit
// values within each 32-bit word are byte-reversed relative to N64 big-endian
// byte order. These macros normalize the access so code works on both ends.
//   E16_IDX(i)  — for short/u16* indices: ((short*)RDRAM)[E16_IDX(i)]
//   E16_ADDR(a) — for byte-offset s16 reads: *(s16*)&RDRAM[E16_ADDR(a)]
//   E8_OFF(o)   — for byte reads: RDRAM[E8_OFF(base + N)]
//   E_XOR(x)    — for template contexts where XOR value is a runtime parameter:
//                  maps LE XOR values (1 for u16, 3 for u8) to 0 on BE
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  #define E16_IDX(idx)  (idx)
  #define E16_ADDR(addr) (addr)
  #define E8_OFF(off)   (off)
  #define E_XOR(x)      (0)
#else
  #define E16_IDX(idx)  ((idx) ^ 1)
  #define E16_ADDR(addr) ((addr) ^ 2)
  #define E8_OFF(off)   ((off) ^ 3)
  #define E_XOR(x)      (x)
#endif

#ifndef TRUE
#define TRUE    1
#endif

#ifndef FALSE
#define FALSE   0
#endif

#ifndef NULL
#define NULL    0
#endif

#ifndef PLUGIN_PATH_SIZE
#define PLUGIN_PATH_SIZE 260
#endif

template <typename T>
class ValueKeeper
{
public:
	ValueKeeper(T& _obj, T _newVal)
		: m_obj(_obj)
		, m_val(_obj)
	{
		m_obj = _newVal;
	}

	~ValueKeeper()
	{
		m_obj = m_val;
	}

private:
	T & m_obj;
	T m_val;
};

#endif // TYPES_H
