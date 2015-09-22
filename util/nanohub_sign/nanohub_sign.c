#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sha2.h>
#include <rsa.h>


static FILE* urandom = NULL;

//read exactly one hex-encoded byte from a file, skipping all the fluff
static int getHexEncodedByte(FILE *f)
{
    int c, i;
    uint8_t val = 0;

    //for first byte
    for (i = 0; i < 2; i++) {
        val <<= 4;
        while(1) {
            c = fgetc(f);
            if (c == EOF)
                return -1;

            if (c >= '0' && c <= '9')
                val += c - '0';
            else if (c >= 'a' && c <= 'f')
                val += c + 10 - 'a';
            else if (c >= 'A' && c <= 'F')
                val += c + 10 - 'A';
            else if (i)                         //disallow everything between first and second nibble
                return -1;
            else if (c > 'f' && c <= 'z')	//disallow nonalpha data
                return -1;
            else if (c > 'F' && c <= 'Z')	//disallow nonalpha data
                return -1;
            else
                continue;
            break;
        }
    }

    return val;
}

//read exactly RSA_BYTES hex encoded bytes from a file, skipping all the fluff
static bool readRsaDataFile(const char *fileName, uint8_t *buf)
{
    FILE *f = fopen(fileName, "rb");
    bool ret = false, haveNonzero = false;
    int byte;
    uint32_t v, i;

    if (!f)
        goto out_noclose;

    for (i = 0; i < RSA_BYTES; i++) {

        //get a byte, skipping all zeroes (openssl likes to prepend one at times)
        do {
            byte = getHexEncodedByte(f);
        } while (byte == 0 && !haveNonzero);
        haveNonzero = true;
        if (byte < 0)
            goto out;

        *buf++ = byte;
    }

    ret = true;

out:
    fclose(f);

out_noclose:
    return ret;
}

//pack a big-endian integer's bytes into our little-endian 32-bit-limbed array
static void binToRsaData(uint32_t *dst, const uint8_t *src)
{
    uint32_t i, j, v;

    for (i = 0; i < RSA_LIMBS; i++) {
        for (v = 0, j = 0; j < 4; j++)
            v = (v << 8) | *src++;
        dst[RSA_LIMBS - i - 1] = v;
    }
}

//provide a random number for which the following property is true ((ret & 0xFF000000) && (ret & 0xFF0000) && (ret & 0xFF00) && (ret & 0xFF))
static uint32_t rand32_no_zero_bytes(void)
{
    uint32_t i, v;
    uint8_t byte;

    if (!urandom) {
        urandom = fopen("/dev/urandom", "rb");
        if (!urandom) {
            fprintf(stderr, "Failed to open /dev/urandom. Cannot procceed!\n");
            exit(-2);
        }
    }

    for (v = 0, i = 0; i < 4; i++) {
        do {
            if (!fread(&byte, 1, 1, urandom)) {
                fprintf(stderr, "Failed to read /dev/urandom. Cannot procceed!\n");
                exit(-3);
            }
        } while (!byte);

        v = (v << 8) | byte;
    }

    return v;
}

static void cleanup(void)
{
    if (urandom)
        fclose(urandom);
}

int main(int argc, char **argv)
{
    const char *selfExeName = argv[0];
    const uint32_t *hash, *rsaResult;
    uint32_t rsanum[RSA_LIMBS];
    uint32_t exponent[RSA_LIMBS];
    uint32_t modulus[RSA_LIMBS];
    struct Sha2state shaState;
    struct RsaState rsaState;
    uint8_t buf[RSA_BYTES];
    uint64_t fileSz = 0;
    bool verbose = false;
    int c, i, ret = -1;

    argc--;
    argv++;

    if (argc >= 1 && !strcmp(argv[0], "-v")) {
        verbose = true;
        argc--;
        argv++;
    }

    if (argc < 1)
        goto usage;

    if (!strcmp(argv[0], "sigdecode")) {
        if (argc < 2)
            goto usage;

        //get modulus
        if (!readRsaDataFile(argv[1], buf)) {
            fprintf(stderr, "failed to read RSA modulus\n");
            goto usage;
        }
        binToRsaData(modulus, buf);

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA modulus: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)modulus[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //read input data as bytes
        i = 0;
        while ((c = getchar()) != EOF) {
            if (i == RSA_BYTES) {
                fprintf(stderr, "too many signature bytes\n");
                goto usage;
            }
            buf[i++] = c;
        }
        if (i != RSA_BYTES) {
            fprintf(stderr, "too few signature bytes\n");
            goto usage;
        }

        //convert into uint32_ts (just read as little endian and pack)
        for (i = 0; i < RSA_LIMBS; i++) {
            uint32_t v, j;

            for (v = 0, j = 0; j < 4; j++)
                v = (v << 8) | buf[i * 4 + 3 - j];

            rsanum[i] = v;
        }

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA cyphertext: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)rsanum[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //do rsa op
        rsaResult = rsaPubOp(&rsaState, rsanum, modulus);

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA plaintext: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)rsaResult[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //verify padding is appropriate and valid
        if ((rsaResult[RSA_LIMBS - 1] & 0xffff0000) != 0x00020000) {
            fprintf(stderr, "Padding header is invalid\n");
            goto out;
        }

        //verify first two bytes of padding
        if (!(rsaResult[RSA_LIMBS - 1] & 0xff00) || !(rsaResult[RSA_LIMBS - 1] & 0xff)) {
            fprintf(stderr, "Padding bytes 0..1 are invalid\n");
            goto out;
        }

        //verify last 3 bytes of padding and the zero terminator
        if (!(rsaResult[8] & 0xff000000) || !(rsaResult[8] & 0xff0000) || !(rsaResult[8] & 0xff00) || (rsaResult[8] & 0xff)) {
            fprintf(stderr, "Padding last bytes & terminator invalid\n");
            goto out;
        }

        //verify middle padding bytes
        for (i = 9; i < RSA_LIMBS - 1; i++) {
            if (!(rsaResult[i] & 0xff000000) || !(rsaResult[i] & 0xff0000) || !(rsaResult[i] & 0xff00) || !(rsaResult[i] & 0xff)) {
                fprintf(stderr, "Padding word %u invalid\n", i);
                goto out;
            }
        }

        //show the hash
        for (i = 0; i < 8; i++)
            printf("%08lx", (unsigned long)rsaResult[i]);
        printf("\n");
        ret = 0;
    }
    else if (!strcmp(argv[0], "sign")) {
        if (argc != 3)
            goto usage;

        //it might not matter, but we still like to try to cleanup after ourselves
        (void)atexit(cleanup);

        //prepare & get RSA info
        if (!readRsaDataFile(argv[1], buf)) {
            fprintf(stderr, "failed to read RSA private exponent\n");
            goto usage;
        }
        binToRsaData(exponent, buf);

        if (!readRsaDataFile(argv[2], buf)) {
            fprintf(stderr, "failed to read RSA modulus\n");
            goto usage;
        }
        binToRsaData(modulus, buf);

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA modulus: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)modulus[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //hash input
        sha2init(&shaState);
        fprintf(stderr, "Reading data to sign...");
        while ((c = getchar()) != EOF) {  //this is slow but our data is small, so deal with it!
            uint8_t byte = c;
            sha2processBytes(&shaState, &byte, 1);
            fileSz++;
        }
        fprintf(stderr, " read %llu bytes\n", (unsigned long long)fileSz);

        //update the user on the progress
        hash = sha2finish(&shaState);
        if (verbose) {
            fprintf(stderr, "SHA2 hash: 0x");
            for (i = 0; i < 8; i++)
                fprintf(stderr, "%08lx", (unsigned long)hash[i]);
            fprintf(stderr, "\n");
        }

        //write into our "data to sign" area
        for (i = 0; i < 8; i++)
            rsanum[i] = hash[i];

        //write padding
        rsanum[i++] = rand32_no_zero_bytes() << 8; //low byte here must be zero as per padding spec
        for (;i < RSA_LIMBS - 1; i++)
            rsanum[i] = rand32_no_zero_bytes();
        rsanum[i] = (rand32_no_zero_bytes() >> 16) | 0x00020000; //as per padding spec

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA plaintext: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)rsanum[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //do the RSA thing
        fprintf(stderr, "Retriculating splines...");
        rsaResult = rsaPrivOp(&rsaState, rsanum, exponent, modulus);
        fprintf(stderr, "DONE\n");

        //update the user
        if (verbose) {
            fprintf(stderr, "RSA cyphertext: 0x");
            for (i = 0; i < RSA_LIMBS; i++)
                fprintf(stderr, "%08lx", (unsigned long)rsaResult[RSA_LIMBS - i - 1]);
            fprintf(stderr, "\n");
        }

        //output in a format that our microcontroller will be able to digest easily & directly (an array of bytes representing little-endian 32-bit words)
        for (i = 0; i < RSA_LIMBS; i++) {
            putchar((rsaResult[i] >>  0) & 0xff);
            putchar((rsaResult[i] >>  8) & 0xff);
            putchar((rsaResult[i] >> 16) & 0xff);
            putchar((rsaResult[i] >> 24) & 0xff);
        }

        fprintf(stderr, "success\n");
        ret = 0;
    }

out:
    return ret;

usage:
    fprintf(stderr, "USAGE: %s [-v] sign <PRIVATE_EXPONENT_FILE> <MODULUS_FILE> < data_to_sign > signature_out\n"
                    "       %s [-v] sigdecode <MODULUS_FILE> < signature_out > expected_sha2\n"
                    "\t<PRIVATE_EXPONENT> and <MODULUS> files contain RSA numbers, big endian format\n",
                    selfExeName, selfExeName);
    return -1;
}



