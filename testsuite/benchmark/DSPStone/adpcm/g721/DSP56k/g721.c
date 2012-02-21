/*
 * g721.c
 * ADPCM transcoder applying the CCITT recommendation G.721
 *
 * It has been successfully tested with all 32kBits reset test sequences.
 * The tests were made on a Sparc Station 10 with various compilers.
 *
 * This source code is released under a DISCLAIMER OF WARRANTY by the
 *
 * Institute for Integrated Systems in Signal Processing
 * Aachen University of Technology
 *
 * For further documentation of the source consult the recommendation.
 *
 * The implementation was done by Juan Martinez Velarde and Chris Schlaeger
 * Last modification 24-Mar-94
 */

#include "g721.h"

#define FALSE 0
#define TRUE (!FALSE)

typedef struct
{
  U16BIT B[6], DQ[6], PK1, PK2, SR1, SR2, A1, A2;
  U16BIT AP, DMS, DML;
  U16BIT YU;
  U16BIT TD;
  U24BIT YL;
} STATES;

/*
 * Prototypes
 */

static void adapt_quant(void);
static void adpt_predict_1(STATES *S);
static void adpt_predict_2(STATES *S);
static void coding_adjustment(void);
static void diff_computation(void);
static U16BIT f_mult(U16BIT An, U16BIT SRn);
static void iadpt_quant(void);
static void input_conversion(void);
static void output_conversion(void);
static void scale_factor_1(STATES* S);
static void scale_factor_2(STATES* S);
static void speed_control_1(STATES* S);
static void speed_control_2(STATES* S);
static void tone_detector_1(STATES* S);
static void tone_detector_2(STATES* S);

/* global signals */
static U16BIT AL, A2P, D, DQ, I, SD, SE, SEZ, S, SL, SR, TDP, TR, Y;

/* ENCODER states */
static STATES E_STATES;
/*{
  0,0,0,0,0,0,32,32,32,32,32,32,0,0,32,32,0,0,
  0,0,0,
  544,0,(U24BIT) 34816L
}; */

/* DECODER states */
static STATES D_STATES;
/*
{
  0,0,0,0,0,0,32,32,32,32,32,32,0,0,32,32,0,0,
  0,0,0,
  544,0,(U24BIT) 34816L
}; */

static U16BIT A_LAW_table[] =
{
   4784,  4752,  4848,  4816,  4656,  4624,  4720,  4688, 
   5040,  5008,  5104,  5072,  4912,  4880,  4976,  4944, 
   4440,  4424,  4472,  4456,  4376,  4360,  4408,  4392, 
   4568,  4552,  4600,  4584,  4504,  4488,  4536,  4520, 
   6848,  6720,  7104,  6976,  6336,  6208,  6592,  6464, 
   7872,  7744,  8128,  8000,  7360,  7232,  7616,  7488, 
   5472,  5408,  5600,  5536,  5216,  5152,  5344,  5280, 
   5984,  5920,  6112,  6048,  5728,  5664,  5856,  5792, 
   4139,  4137,  4143,  4141,  4131,  4129,  4135,  4133, 
   4155,  4153,  4159,  4157,  4147,  4145,  4151,  4149, 
   4107,  4105,  4111,  4109,  4099,  4097,  4103,  4101, 
   4123,  4121,  4127,  4125,  4115,  4113,  4119,  4117, 
   4268,  4260,  4284,  4276,  4236,  4228,  4252,  4244, 
   4332,  4324,  4348,  4340,  4300,  4292,  4316,  4308, 
   4182,  4178,  4190,  4186,  4166,  4162,  4174,  4170, 
   4214,  4210,  4222,  4218,  4198,  4194,  4206,  4202, 
    688,   656,   752,   720,   560,   528,   624,   592, 
    944,   912,  1008,   976,   816,   784,   880,   848, 
    344,   328,   376,   360,   280,   264,   312,   296, 
    472,   456,   504,   488,   408,   392,   440,   424, 
   2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368, 
   3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392, 
   1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184, 
   1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696, 
     43,    41,    47,    45,    35,    33,    39,    37, 
     59,    57,    63,    61,    51,    49,    55,    53, 
     11,     9,    15,    13,     3,     1,     7,     5, 
     27,    25,    31,    29,    19,    17,    23,    21, 
    172,   164,   188,   180,   140,   132,   156,   148, 
    236,   228,   252,   244,   204,   196,   220,   212, 
     86,    82,    94,    90,    70,    66,    78,    74, 
    118,   114,   126,   122,   102,    98,   110,   106
} ;

static U16BIT u_LAW_table[] =
{
  16223, 15967, 15711, 15455, 15199, 14943, 14687, 14431, 
  14175, 13919, 13663, 13407, 13151, 12895, 12639, 12383, 
  12191, 12063, 11935, 11807, 11679, 11551, 11423, 11295, 
  11167, 11039, 10911, 10783, 10655, 10527, 10399, 10271, 
  10175, 10111, 10047,  9983,  9919,  9855,  9791,  9727, 
   9663,  9599,  9535,  9471,  9407,  9343,  9279,  9215, 
   9167,  9135,  9103,  9071,  9039,  9007,  8975,  8943, 
   8911,  8879,  8847,  8815,  8783,  8751,  8719,  8687, 
   8663,  8647,  8631,  8615,  8599,  8583,  8567,  8551, 
   8535,  8519,  8503,  8487,  8471,  8455,  8439,  8423, 
   8411,  8403,  8395,  8387,  8379,  8371,  8363,  8355, 
   8347,  8339,  8331,  8323,  8315,  8307,  8299,  8291, 
   8285,  8281,  8277,  8273,  8269,  8265,  8261,  8257, 
   8253,  8249,  8245,  8241,  8237,  8233,  8229,  8225, 
   8222,  8220,  8218,  8216,  8214,  8212,  8210,  8208, 
   8206,  8204,  8202,  8200,  8198,  8196,  8194,     0, 
   8031,  7775,  7519,  7263,  7007,  6751,  6495,  6239, 
   5983,  5727,  5471,  5215,  4959,  4703,  4447,  4191, 
   3999,  3871,  3743,  3615,  3487,  3359,  3231,  3103, 
   2975,  2847,  2719,  2591,  2463,  2335,  2207,  2079, 
   1983,  1919,  1855,  1791,  1727,  1663,  1599,  1535, 
   1471,  1407,  1343,  1279,  1215,  1151,  1087,  1023, 
    975,   943,   911,   879,   847,   815,   783,   751, 
    719,   687,   655,   623,   591,   559,   527,   495, 
    471,   455,   439,   423,   407,   391,   375,   359, 
    343,   327,   311,   295,   279,   263,   247,   231, 
    219,   211,   203,   195,   187,   179,   171,   163, 
    155,   147,   139,   131,   123,   115,   107,    99, 
     93,    89,    85,    81,    77,    73,    69,    65, 
     61,    57,    53,    49,    45,    41,    37,    33, 
     30,    28,    26,    24,    22,    20,    18,    16, 
     14,    12,    10,     8,     6,     4,     2,     0
} ;

#define LSHIFT(a, b)  ((b) < 0 ? (a) << -(b) : (a) >> (b))
#define SIGNBIT(a, b) ((a) & (((U16BIT) 1) << (b)) ? 1 : 0)
#define MSB(a, b)     { register U16BIT tmp = (U16BIT) (a); \
			(b) = 0; while(tmp) {tmp >>= 1; (b)++; }}

static void adapt_quant()
{
  /*
   * adaptive quantizer
   *
   * Input signals:  D, Y
   * Output signals: I
   */
  U16BIT DS, DL, DLN;

  /* LOG */
  {
    U16BIT DQM, EXP, MANT;

    DS = D >> 15;
    DQM = IF_ELSE(DS, ((U16BIT) (((U24BIT) 65536L) - D)) & 32767, D);
    for (EXP = 0; DQM >> (EXP + 1); EXP++)
      ;
    MANT = ((DQM << 7) >> EXP) & 127;
    DL = (EXP << 7) + MANT;
  }
    	
  /* SUBTB */
  DLN = (DL + 4096 - (Y >> 2)) & 4095;
  
  /* QUAN */
  if (DLN > 3971)
    I = DS ? 0xE : 0x1;
  else if (DLN > 2047)
    I = 0xF;
  else if (DLN > 399)
    I = DS ? 0x8 : 0x7;
  else if (DLN > 348)
    I = DS ? 0x9 : 0x6;
  else if (DLN > 299)
    I = DS ? 0xA : 0x5;     
  else if (DLN > 245)
    I = DS ? 0xB : 0x4;
  else if (DLN > 177)
    I = DS ? 0xC : 0x3;
  else if (DLN > 79)
    I = DS ? 0xD : 0x2;
  else
    I = DS ? 0xE : 0x1;
}

static void adpt_predict_1(STATES *S)
{
  /*
   * adptive predictor
   *
   * Input signals:  none
   * Output signals: SE, SEZ
   * States:         B[], DQ[], A1, A1
   */

  int i;
  U16BIT SEZI, SEI;

  /* FMULT and ACCUM */
  SEZI = 0;
  for (i = 0; i < 6; i++)
    SEZI = (U16BIT) ((SEZI + f_mult(S->B[i], S->DQ[i])) & ((U16BIT) 65535L));
  SEI = (SEZI + f_mult(S->A1, S->SR1) + f_mult(S->A2, S->SR2))
        & ((U16BIT) 65535L);
  SEZ = SEZI >> 1;
  SE = SEI >> 1;
}

static void adpt_predict_2(STATES *S)
{
  /*
   * adaptive predictor
   *
   * Input signals:  DQ, SE, SEZ, TR
   * Output signals: A2P
   * States:         B[], DQ[], PK1, PK2, SR1, SR2, A1, A2
   */

  U16BIT A1P, A1T, A2T, BP[6], PK0, SIGPK, SR0, U[6];

  /* ADDC */
  {
    U16BIT DQS, DQI, SEZS, SEZI, DQSEZ;

    DQS = DQ >> 14;
    DQI = DQS ? 
          (U16BIT) ((((U24BIT) 65536L) - (DQ & 16383)) & ((U16BIT) 65535L))
	  : DQ;
    SEZS = SEZ >> 14;
    SEZI = IF_ELSE(SEZS, (((U16BIT) 1) << 15) + SEZ, SEZ);
    DQSEZ = (DQI + SEZI) & ((U16BIT) 65535L);
    PK0 = DQSEZ >> 15;
    SIGPK = DQSEZ ? 0 : 1;
  }

  /* ADDB */
  {
    U16BIT DQS, DQI, SES, SEI;

    DQS = DQ >> 14;
    DQI = IF_ELSE(DQS, (U16BIT) ((((U24BIT) 65536L) - (DQ & 16383))
                & ((U16BIT)65535L)), DQ);
    SES = SE >> 14;
    SEI = IF_ELSE(SES, (((U16BIT) 1) << 15) + SE, SE);
    SR = (DQI + SEI) & ((U16BIT) 65535L);
  }
  
  /* FLOATB */
  {
    U16BIT SRS, MAG, MANT;
    S16BIT EXP;

    SRS = SR >> 15;
    MAG = IF_ELSE(SRS, (U16BIT) (((((U24BIT) 65536L)) - SR) & 32767), SR);
    MSB(MAG, EXP);
    MANT = MAG ? (MAG << 6) >> EXP : 1 << 5;
    SR0 = (SRS << 10) + (EXP << 6) + MANT;
  }

  /* UPA2 */
  {
    U16BIT PKS1, PKS2, A1S, UGA2S, UGA2, A2S, ULA2, UA2;
    U24BIT UGA2A, FA, FA1, UGA2B;

    PKS1 = PK0 ^ S->PK1;
    PKS2 = PK0 ^ S->PK2;
    UGA2A = PKS2 ? (U24BIT) 114688L : (U24BIT) 16384;
    A1S = S->A1 >> 15;
    if (A1S)
      FA1 = S->A1 >= (U16BIT) 57345L ?
                     (((U24BIT) S->A1) << 2) & ((U24BIT) 131071L) :
		     ((U24BIT) 24577) << 2;
    else
      FA1 = S->A1 <= 8191 ? S->A1 << 2 : 8191 << 2;
    FA = IF_ELSE(PKS1, FA1, (((U24BIT) 131072L) - FA1) & ((U24BIT) 131071L));
    UGA2B = (UGA2A + FA) & ((U24BIT) 131071L);
    UGA2S = (U16BIT) (UGA2B >> 16);
    UGA2 = IF_ELSE(UGA2S == 0 && SIGPK == 0, (U16BIT) (UGA2B >> 7),
           IF_ELSE(UGA2S == 1 && SIGPK == 0, ((U16BIT) (UGA2B >> 7))
                                      + ((U16BIT) 64512L), 0));
    A2S = S->A2 >> 15;
    ULA2 = A2S ? (((U24BIT) 65536L) - ((S->A2 >> 7)
				       + ((U16BIT) 65024L)))
		  & ((U16BIT) 65535L) :
                 (((U24BIT) 65536L) - (S->A2 >> 7)) & ((U16BIT) 65535L);
    UA2 = (UGA2 + ULA2) & ((U16BIT) 65535L);
	  A2T = (S->A2 + UA2) & ((U16BIT) 65535L);
  }

  /* LIMC */
  A2P = IF_ELSE(((U16BIT) 32768L) <= A2T && A2T <= ((U16BIT) 53248L),
        ((U16BIT) 53248L),
        IF_ELSE(12288 <= A2T && A2T <= 32767, 12288, A2T));

  /* UPA1 */
  {
    U16BIT PKS, UGA1, A1S, ULA1;

    PKS = PK0 ^ S->PK1;
    UGA1 = IF_ELSE(PKS == 0 && SIGPK == 0, 192,
           IF_ELSE(PKS == 1 && SIGPK == 0, (U16BIT) 65344L, 0));
    A1S = S->A1 >> 15;
    ULA1 = (U16BIT) ((A1S ? (((U24BIT) 65536L) - ((S->A1 >> 8)
	         + ((U16BIT) 65280L))) :
                 (((U24BIT) 65536L) - (S->A1 >> 8)))
           & ((U16BIT) 65535L));
    A1T = (S->A1 + UGA1 + ULA1) & ((U16BIT) 65535L);
  }

  /* LIMD */
  {
    U16BIT A1UL, A1LL;

    A1UL = (U16BIT) ((((U24BIT) 80896L) - A2P) & ((U16BIT) 65535L));
    A1LL = (A2P + ((U16BIT) 50176L)) & ((U16BIT) 65535L);
    A1P = ((U16BIT) 32768L) <= A1T && A1T <= A1LL ? A1LL :
          A1UL <= A1T && A1T <= 32767 ? A1UL : A1T;
  }

  /* XOR */
  {
    int i;
    U16BIT DQS;

    DQS = DQ >> 14;
    for (i = 0; i < 6; i++)
      U[i] = DQS ^ (S->DQ[i] >> 10);
  }

  /* UPB */
  {
    int i;
    U16BIT DQMAG, UGB, BS, ULB, UB;

    for ( i = 0; i < 6; i++)
    {
      DQMAG = DQ & 16383;
      UGB = IF_ELSE(U[i] == 0 && DQMAG != 0, 128,
	    IF_ELSE(U[i] == 1 && DQMAG != 0, ((U16BIT) 65408L), 0));
      BS = S->B[i] >> 15;
      ULB = (U16BIT) ((BS ?
                      (((U24BIT) 65536L) - ((S->B[i] >> 8)
                        + ((U16BIT) 65280L))) :
	              (((U24BIT) 65536L) - (S->B[i] >> 8)))
		      & ((U16BIT) 65535L));
      UB = (UGB + ULB) & ((U16BIT) 65535L);
      BP[i] = (S->B[i] + UB) & ((U16BIT) 65535L);
    }
  }

  /* TRIGB */
  {
    int i;

    if (TR)
    {
      S->A1 = S->A2 = 0;
      for (i = 0; i < 6; i++)
	S->B[i] = 0;
    }
    else
    {
      S->A1 = A1P;
      S->A2 = A2P;
      for (i = 0; i < 6; i++)
	S->B[i] = BP[i];
    }
  }

  /* FLOATA */
  {
    int i = 0;
    U16BIT DQS, MAG, MANT;
    S16BIT EXP;

    for (i = 5; i; i--)
      S->DQ[i] = S->DQ[i - 1];

    DQS = DQ >> 14;
    MAG = DQ & 16383;
    MSB(MAG, EXP);
    MANT = MAG ? (MAG << 6) >> EXP : 1 << 5;
    S->DQ[0] = (DQS << 10) + (EXP << 6) + MANT;
  }

  S->PK2 = S->PK1;
  S->PK1 = PK0;
  S->SR2 = S->SR1;
  S->SR1 = SR0;
}

static void coding_adjustment()
{
  /*
   * synchronous coding adjustment
   *
   * Input signals:  D, SP, Y, LAW, I
   * Output signals: SD
   */

  U16BIT DL, DLN, DS;

  /* LOG */
  {
    U16BIT DQM, EXP, MANT;

    DS = D >> 15;
    DQM = IF_ELSE(DS, (U16BIT) (((U24BIT) 65536L) - D) & 32767, D);
    for (EXP = 0; DQM >> (EXP + 1); EXP++)
      ;
    MANT = ((DQM << 7) >> EXP) & 127;
    DL = (EXP << 7) + MANT;
  }
    	
  /* SUBTB */
  DLN = (DL + 4096 - (Y >> 2)) & 4095;

  /* SYNC */
  {
    U16BIT IS, IM, ID;

    IS = I >> 3;
    IM = IS ? I & 7 : I + 8;
    if (DLN < 80)
      ID = DS ? 6 : 9;
    else if (DLN < 178)
      ID = DS ? 5 : 10;
    else if (DLN < 246)
      ID = DS ? 4 : 11;
    else if (DLN < 300)
      ID = DS ? 3 : 12;
    else if (DLN < 349)
      ID = DS ? 2 : 13;
    else if (DLN < 400)
      ID = DS ? 1 : 14;
    else if (DLN < 2048)
      ID = DS ? 0 : 15;
    else if (DLN < 3972)
      ID = 7;
    else
      ID = DS ? 6 : 9;

    if (LAW)
    {
      SD = S ^ 0x55;
      if (ID > IM)
      {
	if (SD <= 126)
	  SD++;
        else if (SD >= 129)
	  SD--;
	else
	  SD = SD == 128 ? 0 : 127;
      }
      else if (ID < IM)
      {
        if (SD >= 1 && SD <= 127)
	  SD--;
        else if (SD >= 128 && SD <= 254)
	  SD++;
	else
	  SD = SD ? 255 : 128;
      }
      SD ^= 0x55;
    }
    else
    {
      if (ID > IM)
      {
	if (1 <= S && S <= 127)
	  SD = S - 1;
	else if (128 <= S && S <= 254)
	  SD = S + 1;
	else
	  SD = S ? 126 : 0;
      }
      else if (ID < IM)
      {
	if (S <= 126)
	  SD = S + 1;
	else if (128 < S && S <= 255)
	  SD = S - 1;
	else
	  SD = S == 127 ? 254 : 128;
      }
      else
	SD = S;
    }
  }
}

static void diff_computation()
{
  /*
   * difference signal computation
   *
   * Input signals:  SL, SE
   * Output signals: D
   */
  U16BIT SLS, SLI, SES, SEI;

  /* SUBTA */
  SLS = SL >> 13;
  SLI = IF_ELSE(SLS, ((U16BIT) 49152L) + SL, SL);
  SES = SE >> 14;
  SEI = IF_ELSE(SES, ((U16BIT) 32768L) + SE, SE);
  D = (SLI + (((U24BIT) 65536L) - SEI)) & ((U16BIT) 65535L);
}

static U16BIT f_mult(U16BIT An, U16BIT SRn)
{
  U16BIT AnS, AnMAG, AnMANT, SRnS, SRnEXP, SRnMANT, WAnS, WAnEXP, WAnMAG;
  U16BIT AnEXP;
  U24BIT WAnMANT;

  AnS = An >> 15;
  AnMAG = AnS ? (16384 - (An >> 2)) & 8191 : An >> 2;
  MSB(AnMAG, AnEXP);
  AnMANT = AnMAG ? (((U24BIT) AnMAG) << 6) >> AnEXP : 1 << 5;
  SRnS = SRn >> 10;
  SRnEXP = (SRn >> 6) & 15;
  SRnMANT = SRn & 63;
  WAnS = SRnS ^ AnS;
  WAnEXP = SRnEXP + AnEXP;
  WAnMANT = (((U24BIT) SRnMANT * AnMANT) + 48) >> 4;
  WAnMAG = WAnEXP <= 26 ?
           (WAnMANT << 7) >> (26 - WAnEXP) :
	   ((WAnMANT << 7) << (WAnEXP - 26)) & 32767;
  return (IF_ELSE(WAnS, ((((U24BIT) 65536L) - WAnMAG)) & ((U16BIT) 65535L)
	  , WAnMAG));
}

static void iadpt_quant()
{
  /*
   * inverse adaptive quantizer
   * 
   * Input signals:  I, Y
   * Output signals: DQ
   */
 
  static U16BIT qtab[] =
  {
    2048, 4, 135, 213, 273, 323, 373, 425,
    425, 373, 323, 273, 213, 135, 4, 2048,
  } ;
  U16BIT DQL;

  /* RECONST and ADDA */
  DQL = (qtab[I] + (Y >> 2)) & 4095;

  /* ANTILOG */
  {
    U16BIT DS, DEX, DMN, DQT, DQMAG;

    DS = DQL >> 11;
    DEX = (DQL >> 7) & 15;
    DMN = DQL & 127;
    DQT = (1 << 7) + DMN;
    DQMAG = DS ? 0 : (DQT << 7) >> (14 - DEX);
    DQ = ((I >> 3) << 14) + DQMAG;
  }
}

static void input_conversion()
{
  /*
   * convert to uniform PCM
   * Input signals:  S
   * Output signals: SL
   */

  U16BIT SS, SSS, SSM, SSQ;

  /* EXPAND */
  if (LAW)
  {
    SS = A_LAW_table[S];
    SSS = SS >> 12;
    SSM = SS & 4095;
    SSQ = SSM << 1;
  }
  else
  {
    SS = u_LAW_table[S];
    SSS = SS >> 13;
    SSQ = SS & 8191;
  }
  SL = IF_ELSE(SSS, (16384 - SSQ) & 16383, SSQ);
}

static void output_conversion()
{
  /*
   * Output PCM format conversion
   *
   * Input signals:  SR
   * Output signals: S
   */
  U16BIT IS, IM, IMAG;

  IS = SR >> 15;
  IM = IF_ELSE(IS, ((U16BIT) (((U24BIT) 65536L) - SR) & 32767), SR);
  IMAG = IF_ELSE(LAW == 0, IM, IF_ELSE(IS, (IM + 1) >> 1, IM >> 1));

  if (LAW)
  {
    U16BIT MASK, SEG, IMS;

    MASK = IS ? 0x55 : 0xD5;
    IMS = IMAG - (IS ? 1 : 0);
    if (IMS > 4095)
      S = 0x7F ^ MASK;
    else
    {
      for (SEG = 5; IMS >> SEG; SEG++)
	;
      SEG -= 5;
      S = (SEG << 4 | ((SEG ? IMS >> SEG : IMS >> 1) & 0xF)) ^ MASK;
    }
  }
  else
  {
    U16BIT MASK, IMS, SEG;

    MASK = IS ? 0x7F : 0xFF;
    IMS = IMAG + 33;
    if (IMS > 8191)
      S = 0x7F ^ MASK;
    else
    {
      for (SEG = 5; IMS >> SEG; SEG++)
	;
      SEG -= 6;
      S = (SEG << 4 | ((IMS >> SEG + 1) & 0xF)) ^ MASK;
    }
  }
}

static void scale_factor_1(STATES* S)
{
  /*
   * quantizer scale factor adaptation (part 1)
   * 
   * Input signals:  AL
   * Output signals: Y
   * States:         YU, YL
   */

  /* MIX */
  {
    U16BIT DIF, DIFS, DIFM, PRODM, PROD;

    DIF = (S->YU + 16384 - ((U16BIT) (S->YL >> 6))) & 16383;
    DIFS = DIF >> 13;
    DIFM = IF_ELSE(DIFS, (16384 - DIF) & 8191, DIF);
    PRODM = (DIFM * AL) >> 6;
    PROD = IF_ELSE(DIFS, (16384 - PRODM) & 16383, PRODM);
    Y = (((U16BIT) (S->YL >> 6)) + PROD) & 8191;
  }
}

static void scale_factor_2(STATES* S)
{
  /*
   * quantizer scale factor adaptation
   *
   * Input signals:  I
   * Output signals: Y, S->YL
   * States:         YL, YU
   */

  static U16BIT W[] = 
  {
    4084, 18, 41, 64, 112, 198, 355, 1122
  } ;
  U16BIT WI, YUT;

  /* FUNCTW */
  WI = W[(I >> 3) ? (15 - I) & 7 : I & 7];
  
  /* FILTD */
  {
    U16BIT DIFS, DIFSX;
    U24BIT DIF;

    DIF = ((WI << 5) + ((U24BIT) 131072L) - Y) & ((U24BIT) 131071L);
    DIFS = DIF >> 16;
    DIFSX = DIFS ? (DIF >> 5) + 4096 : DIF >> 5;
    YUT = (Y + DIFSX) & 8191;
  }

  /* LIMB */
  {
    U16BIT GEUL, GELL;

    GEUL = ((YUT + 11264) & 16383) >> 13;
    GELL = ((YUT + 15840) & 16383) >> 13;
    S->YU = IF_ELSE(GELL == 1, 544, IF_ELSE(GEUL == 0, 5120, YUT));
  }

  /* FILTE */
  {
    U16BIT DIF, DIFS;
    U24BIT DIFSX;

    DIF = (U16BIT) (S->YU + ((((U24BIT) 1048576L) - S->YL) >> 6)) & 16383;
    DIFS = DIF >> 13;
    DIFSX = IF_ELSE(DIFS, DIF + ((U24BIT) 507904L), DIF);
    S->YL = (S->YL + DIFSX) & ((U24BIT) 524287L);
  }
}

static void speed_control_1(STATES* S)
{
  /*
   * adaption speed control
   *
   * Input signals:  none
   * Output signals: AL
   * States:         AP
   */

  /* LIMA */
  AL = S->AP > 255 ? 64 : S->AP >> 2;
}

static void speed_control_2(STATES* S)
{
  /*
   * adaption speed control
   *
   * Input signals:  TR, TDP, I, Y
   * Output signals: none
   * States:         AP, DMS, DML
   */

  static U16BIT F[] = { 0, 0, 0, 1, 1, 1, 3, 7 };
  U16BIT FI, AX, APP;

  /* FUNTCF */
  FI = F[IF_ELSE(I >> 3, (15 - I), I) & 7] ; 

  /* FILTA */
  {
    U16BIT DIF, DIFS, DIFSX;

    DIF = ((FI << 9) + 8192 - S->DMS) & 8191;
    DIFS = DIF >> 12;
    DIFSX = DIFS ? (DIF >> 5) + 3840 : DIF >> 5;
    S->DMS = (DIFSX + S->DMS) & 4095;
  }

  /* FILTB */
  {
    U16BIT DIF, DIFS, DIFSX;

    DIF = ((FI << 11) + ((U16BIT) 32768L) - S->DML) & 32767;
    DIFS = DIF >> 14;
    DIFSX = DIFS ? (DIF >> 7) + 16128 : DIF >> 7;
    S->DML = (DIFSX + S->DML) & 16383;
  }

  /* SUBTC */
  {
    U16BIT DIF, DIFS, DIFM, DTHR;

    DIF = ((S->DMS << 2) + ((U16BIT) 32768L) - S->DML) & 32767;
    DIFS = DIF >> 14;
    DIFM = IF_ELSE(DIFS, (((U16BIT) 32768L) - DIF) & 16383, DIF);
    DTHR = S->DML >> 3;
    AX = (Y >= 1536 && DIFM < DTHR && TDP == 0) ? 0 : 1;
  }

  /* FILTC */
  {
    U16BIT DIF, DIFS, DIFSX;

    DIF = ((AX << 9) + 2048 - S->AP) & 2047;
    DIFS = DIF >> 10;
    DIFSX = DIFS ? (DIF >> 4) + 896 : DIF >> 4;
    APP = (DIFSX + S->AP) & 1023;
  }

  /* TRIGA */
  S->AP = IF_ELSE(TR, 256, APP);
}

static void tone_detector_1(STATES* S)
{
  /*
   * tone and transition detector
   *
   * Input signals:  S->YL, DQ
   * Output signals: TR
   * STATES:         YL, TD;
   */

  /* TRANS */
  {
    U16BIT DQMAG, YLINT, YLFRAC, THR1, THR2, DQTHR;

    DQMAG = DQ & 16383;
    YLINT = (U16BIT) (S->YL >> 15);
    YLFRAC = ((U16BIT) (S->YL >> 10)) & 31;
    THR1 = (32 + YLFRAC) << YLINT;
    THR2 = IF_ELSE(YLINT > 8, 31 << 9, THR1);
    DQTHR = (THR2 + (THR2 >> 1)) >> 1;
    TR = DQMAG > DQTHR && S->TD == 1 ? 1 : 0;
  }
}

static void tone_detector_2(STATES* S)
{
  /*
   * tone and transition detector
   *
   * Input signals:  TR, S->A2
   * Output signals: TDP;
   * States:         TD;
   */


  /* TONE */
  TDP = ((U16BIT) 32768L) <= A2P && A2P < ((U16BIT ) 53760L) ? 1 : 0;

  /* TRIGB */
  S->TD = IF_ELSE(TR, 0, TDP);
}

/***************************** public part ***********************************/

int   LAW = u_LAW;

void reset_encoder(void)
{
  int i;

  for (i = 0; i < 6; i++)
    E_STATES.B[i] = 0;
  for (i = 0; i < 6; i++)
    E_STATES.DQ[i] = 32;
  E_STATES.PK1 = E_STATES.PK2 = 0;
  E_STATES.SR1 = E_STATES.SR2 = 32;
  E_STATES.A1 = E_STATES.A2 = 0;
  E_STATES.AP = E_STATES.DMS = E_STATES.DML = 0;
  E_STATES.YU = 544;
  E_STATES.TD = 0;
  E_STATES.YL = (U24BIT) 34816L;
}

U16BIT encoder(U16BIT pcm)
{
  S = pcm;

  input_conversion();
  adpt_predict_1(&E_STATES);
  diff_computation();
  speed_control_1(&E_STATES);
  scale_factor_1(&E_STATES);
  adapt_quant();
 
  iadpt_quant();
  tone_detector_1(&E_STATES);
  adpt_predict_2(&E_STATES);
  tone_detector_2(&E_STATES);
  scale_factor_2(&E_STATES);
  speed_control_2(&E_STATES);

  return (I);
}

void reset_decoder(void)
{
  int i;

  for (i = 0; i < 6; i++)
    D_STATES.B[i] = 0;
  for (i = 0; i < 6; i++)
    D_STATES.DQ[i] = 32;
  D_STATES.PK1 = D_STATES.PK2 = 0;
  D_STATES.SR1 = D_STATES.SR2 = 32;
  D_STATES.A1 = D_STATES.A2 = 0;
  D_STATES.AP = D_STATES.DMS = D_STATES.DML = 0;
  D_STATES.YU = 544;
  D_STATES.TD = 0;
  D_STATES.YL = (U24BIT) 34816L;
}

U16BIT decoder(U16BIT adpcm)
{

  I = adpcm;

  speed_control_1(&D_STATES);
  scale_factor_1(&D_STATES);
  iadpt_quant();
  tone_detector_1(&D_STATES);
  adpt_predict_1(&D_STATES);
  adpt_predict_2(&D_STATES);
  tone_detector_2(&D_STATES);
  scale_factor_2(&D_STATES);
  speed_control_2(&D_STATES);
  output_conversion();
  input_conversion();
  diff_computation();
  coding_adjustment();

  return (SD);
}
