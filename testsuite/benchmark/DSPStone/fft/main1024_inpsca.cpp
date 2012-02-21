/*
 *  benchmark program  : fft.c (main program)
 *                       fft_inpsca.c
 * 
 *  benchmark suite    : DSP-kernel
 *
 *  description        : benchmarking of an integer input scaled FFT 
 *
 *                      To avoid errors caused by overflow and bit growth, 
 *                      the input data is scaled. Bit growth occurs potentially
 *                      at butterfly operations, which involve a complex 
 *                      multiplication, a complex addition and a complex 
 *                      subtraction. Maximal bit growth from butterfly input 
 *                      to butterfly output is two bits. 
 *
 *                      The input data includes enough extra sign bits, called 
 *                      guard bits, to ensure that bit growth never results in 
 *                      overflow.
 *
 *                      The number of guard bits necessary to compensate the 
 *                      maximum bit growth in an N-point FFT is (log_2 (N))+1).
 * 
 *                      In a 16-point FFT (requires 4 stages), each of the 
 *                      input samples must contain 5 guard bits. Indeed, the 
 *                      input data is restricted to 9 bits, in order to prevent
 *                      a overflow from the integer multiplication with the
 *                      7 bit precalculed twiddle coefficients.
 *                     
 *                      Input data is held on the include file "input1024.dat"
 *                      in float format (0 ... 1)
 *                      Data is transformed automatically to 1.9 fract format
 *
 *  reference code     : none
 *
 *  func. verification : comparison with known float 1024 point FFT
 *
 *  organization       : Aachen University of Technology - IS2
 *                     : DSP Tools Group
 *                     : phone   : +49(241)807887
 *                     : fax     : +49(241)8888195
 *                     : e-mail  : zivojnov@ert.rwth-aachen.de
 *
 *  author             : Juan Martinez Velarde
 *
 *  history            : 07-02-94 - creation
 *                       16-02-94 - c50 profiling
 *
 */
#ifdef __cplusplus
extern "C" {
#endif

#include"stdio.h"
#include"include/main1024_inpsca.h"

#include "include/input_data/twids1024-7.dat"   /* precalculated twiddle factors 
                            for an integer 1024 point FFT 
                            in format 1.7 => table twidtable[2*(N_FFT-1)] ; */

#include "include/input_data/input1024.dat"    /* 1024 real values as
                             input data in float format */

#include "include/convert.c"   /* conversion function to 1.NUMBER_OF_BITS format */


void
main1024_inpsca(STORAGE_CLASS TYPE *int_pointer)
     //STORAGE_CLASS TYPE *int_pointer ;
{
  #ifdef __PROF56__
  t = __time;
  #endif
  
  //START_PROFILING; 
  
  {
    STORAGE_CLASS TYPE i, j = 0  ; 
    STORAGE_CLASS TYPE tmpr, max = 2, m, n = N_FFT << 1 ; 
    
    /* do the bit reversal scramble of the input data */
    
    for (i = 0; i < (n-1) ; i += 2) 
      {
	if (j > i)
	  {
	    tmpr = *(int_pointer + j) ;
	    *(int_pointer + j) = *(int_pointer + i) ;
	    *(int_pointer + i) = tmpr ; 
	    
	    tmpr = *(int_pointer + j + 1) ; 
	    *(int_pointer + j + 1) = *(int_pointer + i + 1) ; 
	    *(int_pointer + i + 1) = tmpr ; 
	  }
	
	m = N_FFT;
	while (m >= 2 && j >= m) 
	  {
	    j -= m ;
	    m >>= 1;
	  }
	j += m ;
      }
    
    {
      STORAGE_CLASS TYPE *data_pointer = &twidtable[0] ; 
      STORAGE_CLASS TYPE *p, *q ; 
      STORAGE_CLASS TYPE tmpi, fr = 0, level, k, l ; 
      
      while (n > max)
	{      
	  level = max << 1 ;
	  for (m = 1; m < max; m += 2) 
	    {
	      l = *(data_pointer + fr) ; 
	      k = *(data_pointer + fr + 1) ;
	      fr += 2 ; 
	      
	      for (i = m; i <= n; i += level) 
		{
		  j = i + max;
		  p = int_pointer + j ; 
		  q = int_pointer + i ; 
		  
		  tmpr  = l * *(p-1) ; 
		  tmpr -= (k * *p ) ; 
		  
		  tmpi  = l * *p  ; 
		  tmpi += (k * *(p-1)) ; 
		  
		  tmpr  = tmpr >> SHIFT ; 
		  tmpi  = tmpi >> SHIFT ; 
		  
		  *(p-1) = *(q-1) - tmpr ; 
		  *p     = *q - tmpi ; 
		  
		  *(q-1) += tmpr ;
		  *q     += tmpi ; 
		}
	    }      
	  max = level;
	}
    }
    
  }
  
  //END_PROFILING;   
  
  #ifdef __PROF56__
  t = __time - t;
  printf("time = %d\n", t);
  #endif
  
}

#ifdef __cplusplus
}
#endif

void
pin_down(TYPE input_data[])
     //TYPE input_data[] ; 
{

#ifdef __DEBUG__
  pin_down_debug();
#endif
  
  /* conversion from input1024.dat to a 1.9 format */
  
  float2fract() ; 
  
  {
    int          *pd, *ps, f  ;
    
    pd = &input_data[0];
    ps = &inputfract[0] ; 
    
    for (f = 0; f < N_FFT; f++) 
      {
	*pd++ = *ps++  ; /* fill in with real data */
	*pd++ = 0 ;      /* imaginary data is equal zero */
      }    
  }
}


TYPE 
main()
{
  STORAGE_CLASS TYPE input_data[2*N_FFT]; 
  int j;
  pin_down(&input_data[0]) ;
  
  main1024_inpsca(&input_data[0]);  
  
  for(j=0;j<(2*N_FFT);j++)
	printf("input_data[%d]: %d\n",j,input_data[j]);
  
#ifdef __DEBUG__
  dump_to_file(&input_data[0]);
#endif
  
  return (0) ; 
  
}

