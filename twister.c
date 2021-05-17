#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gmp.h>

// This is the ``Mersenne Twister'' random number generator MT19937
// http://www.math.keio.ac.jp/~matumoto/ver980409.html
// with inspiration from code attributed to Cokus@math.washington.edu

#define N (624)                              // length of state vector
#define M (397)                              // a period parameter
#define K (0x9908B0DFU)                      // a magic constant
#define hiBit(u) ((u)&0x80000000U)           // mask all but highest   bit of u
#define loBit(u) ((u)&0x00000001U)           // mask all but lowest    bit of u
#define loBits(u) ((u)&0x7FFFFFFFU)          // mask     the highest   bit of u
#define mixBits(u, v) (hiBit(u) | loBits(v)) // move hi bit of u to hi bit of v

static inline uint32_t temperMT(uint32_t s1)
{
  s1 ^= (s1 >> 11);
  s1 ^= (s1 << 7) & 0x9D2C5680U;
  s1 ^= (s1 << 15) & 0xEFC60000U;
  return (s1 ^ (s1 >> 18));
}

void seedMT(uint32_t seed, uint32_t *output)
{
  uint32_t state[N + 1];
  uint32_t *next;

  uint32_t x = (seed | 1U) & 0xFFFFFFFFU, *s = state;
  int j;

  for (*s++ = x, j = N; --j;
       *s++ = (x *= 69069U) & 0xFFFFFFFFU)
    ;
  uint32_t *p0 = state, *p2 = state + 2, *pM = state + M, s0, s1;

  next = state + 1;

  for (s0 = state[0], s1 = state[1], j = N - M + 1; --j; s0 = s1, s1 = *p2++)
    *p0++ = *pM++ ^ (mixBits(s0, s1) >> 1) ^ (loBit(s1) ? K : 0U);

  for (pM = state, j = M; --j; s0 = s1, s1 = *p2++)
    *p0++ = *pM++ ^ (mixBits(s0, s1) >> 1) ^ (loBit(s1) ? K : 0U);

  s1 = state[0], *p0 = *pM ^ (mixBits(s0, s1) >> 1) ^ (loBit(s1) ? K : 0U);
  output[0] = temperMT(s1);
  uint32_t y;
  for (int i = 1; i < 32; i++)
  {
    y = *next++;
    output[i] = temperMT(y);
  }
}

static inline uint32_t bswap32(uint32_t x)
{
  return (((x & 0xff000000u) >> 24) |
          ((x & 0x00ff0000u) >> 8) |
          ((x & 0x0000ff00u) << 8) |
          ((x & 0x000000ffu) << 24));
}

int main(int argc, char *argv[])
{
  if (argc < 3)
  {
    return 1;
  }
  uint32_t seed = strtol(argv[1], NULL, 16);
  uint64_t match = strtoll(argv[2], NULL, 16);
  int suppress_output = 0;
  if (argc > 3)
  {
    suppress_output = 1;
  }

  mpz_t n;
  mpz_init_set_str(n, "c18ca0cfd924257f0f42ddae1871c42e93fcb04cb5a975bb1f320f18ad8846487c1aa961a72c456555d346f660ea27318053b78f1066870a1938c897bf24cd36a2fef7cd8bc096b32f52a89be8dccfa89351ee0351f664ea2169a9b96488997db58036fa650be0e3c66bb2ec4dfe74f0eb22842cc98e7a1974865ca72d3730e5", 16);
  mpz_t e;
  mpz_init_set_ui(e, 65537U);

  int done = 0;
#pragma omp parallel shared(seed, done, match, n, e, suppress_output)
  while (!done)
  {
    int j = 0;
    uint32_t current_seed = 0;
#pragma omp atomic capture
    {
      current_seed = seed;
      seed = seed + 2;
    }

    uint32_t rand_data[32];
    seedMT(current_seed, rand_data);
    unsigned char *rand_data_bytes = (unsigned char *)rand_data;

    // The last int has the high bits replaced with 0x200, presumably to make the resulting bigint valid within the RSA parameters.
    rand_data[31] = bswap32(bswap32(rand_data[31] & 0xFFFF) + 0x0200);

    // This byte is just straight up set to 0, at 800167d4 in SBOOT, who knows why...
    rand_data_bytes[117] = 0;

    mpz_t data_num;
    mpz_init(data_num);
    mpz_import(data_num, 32, -1, 4, -1, 0, rand_data_bytes);
    mpz_t output;
    mpz_init(output);
    mpz_powm(output, data_num, e, n);
    unsigned char rsa_output[128];
    mpz_export(rsa_output, NULL, -1, 4, -1, 0, output);

    uint32_t *rsa_output_ints = (uint32_t *)rsa_output;

    if (rsa_output_ints[0] == match)
    {
      done = 1;
      if (!suppress_output)
      {
        printf("**** FOUND ****\n");
        printf("Seed: %08X\n", current_seed);
        printf("\nKey Data: \n");
      }
      for (j = 0; j < 32; j++)
      {
        printf("%08X", rand_data[j]);
      }
      if (!suppress_output)
      {

        printf("\nSeed Data: \n");
        for (j = 0; j < 32; j++)
        {
          printf(" %08X%s", rsa_output_ints[j], (j % 4) == 4 ? " " : "");
        }
        printf("\n");
      }
    }
  }

  return (EXIT_SUCCESS);
}
