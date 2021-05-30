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
  for (int i = 1; i < 64; i++)
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
  #ifdef SIMOS1810
  mpz_init_set_str(n, "b21fd93ed14799a3f3f1db1db0159c385537e81397af0f1908d35a08115dabc13c5580833d82d884f9b4c09127df248e8a7ca815c952c8bbfda097d5b6e56a4072a2842713032229ae55a7307215b8e48c1b7b883a18d7f90e81c766fa8f5f2194efdcfd79fa5174f5a92f5bba5dabb4e2c138c0a5aab35032ecb90eb73ffe0c1ae3d209f421de40ab8363897c0809d93938ebed3e7b25230fdf38795c4e467cccaca5ffb52d49b7130e13aab881fa280c6df9dd4ac7ba0038985064517b6f96d5abef5be3e21cf261068b94fb128cd98cf02a2033c0e1f57b887e401d041f8afc891727d1d04f14422eefd08c4b925e45e2446ec98acaefb42c313e2abae9d5", 16);
  #else
  mpz_init_set_str(n, "de5a5615fdda3b76b4ecd8754228885e7bf11fdd6c8c18ac24230f7f770006cfe60465384e6a5ab4daa3009abc65bff2abb1da1428ce7a925366a14833dcd18183bad61b2c66f0d8b9c4c90bf27fe9d1c55bf2830306a13d4559df60783f5809547ffd364dbccea7a7c2fc32a0357ceba3e932abcac6bd6398894a1a22f63bdc45b5da8b3c4e80f8c097ca7ffd18ff6c78c81e94c016c080ee6c5322e1aeb59d2123dce1e4dd20d0f1cdb017326b4fd813c060e8d2acd62e703341784dca667632233de57db820f149964b3f4f0c785c39e2534a7ae36fd115b9f06457822f8a9b7ce7533777a4fb03610d6b4018ab332be4e7ad2f4ac193040e5a037417bc53", 16);
  #endif
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

    uint32_t rand_data[64];
    seedMT(current_seed, rand_data);
    unsigned char *rand_data_bytes = (unsigned char *)rand_data;

    // The last int has the high bits replaced with 0x200, presumably to make the resulting bigint valid within the RSA parameters.
    rand_data[63] = bswap32(bswap32(rand_data[63] & 0xFFFF) + 0x0200);

    // This byte is just straight up set to 0, at 800167d4 in SBOOT, who knows why...
    rand_data_bytes[245] = 0;

    mpz_t data_num;
    mpz_init(data_num);
    mpz_import(data_num, 64, -1, 4, -1, 0, rand_data_bytes);
    mpz_t output;
    mpz_init(output);
    mpz_powm(output, data_num, e, n);
    unsigned char rsa_output[256];
    mpz_export(rsa_output, NULL, -1, 4, -1, 0, output);
    mpz_clear(output);
    mpz_clear(data_num);

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
      for (j = 0; j < 64; j++)
      {
        printf("%08X", rand_data[j]);
      }
      if (!suppress_output)
      {

        printf("\nSeed Data: \n");
        for (j = 0; j < 64; j++)
        {
          printf(" %08X%s", rsa_output_ints[j], (j % 4) == 4 ? " " : "");
        }
        printf("\n");
      }
    }
  }
  mpz_clear(e);
  mpz_clear(n);

  return (EXIT_SUCCESS);
}
