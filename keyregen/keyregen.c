#include "bsdtar_platform.h"

#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "crypto.h"
#include "humansize.h"
#include "keyfile.h"
#include "netpacket.h"
#include "netproto.h"
#include "network.h"
#include "readpass.h"
#include "sysendian.h"
#include "tarsnap_opt.h"
#include "warnp.h"

struct register_internal {
	/* Parameters provided from main() to network code. */
	const char * user;
	char * passwd;
	const char * name;

	/* State information. */
	int donechallenge;
	int done;

	/* Key used to send challenge response and verify server response. */
	uint8_t register_key[32];

	/* Data returned by server. */
	uint8_t status;
	uint64_t machinenum;
};

static sendpacket_callback callback_register_send;
static handlepacket_callback callback_register_challenge;
static handlepacket_callback callback_register_response;
static void usage(void);

/* Be noisy about network errors while registering a machine. */
int tarsnap_opt_noisy_warnings = 1;

static void
usage(void)
{

	fprintf(stderr, "usage: tarsnap-keyregen %s %s %s %s %s %s\n",
	    "--keyfile key-file", "--oldkey old-key-file",
	    "--user user-name", "--machine machine-name",
	    "[--passphrased]", "[--passphrase-mem maxmem]");
	exit(1);

	/* NOTREACHED */
}

int
main(int argc, char **argv)
{
	struct register_internal C;
	const char * keyfilename;
	const char * oldkeyfilename;
	FILE * keyfile;
	struct stat sb;
	NETPACKET_CONNECTION * NPC;
	int passphrased;
	uint64_t maxmem;
	char * passphrase;
	uint64_t dummy;

#ifdef NEED_WARN_PROGNAME
	warn_progname = "tarsnap-rekeygen";
#endif

	/*
	 * We have no username, machine name, key filename, or old key
	 * filename yet.
	 */
	C.user = C.name = NULL;
	keyfilename = NULL;
	oldkeyfilename = NULL;

	/* We're not using a passphrase, and have unlimited RAM so far. */
	passphrased = 0;
	maxmem = 0;

	/* Parse arguments. */
	while (--argc > 0) {
		argv++;

		if (strcmp(argv[0], "--user") == 0) {
			if ((C.user != NULL) || (argc < 2))
				usage();
			C.user = argv[1];
			argv++; argc--;
		} else if (strcmp(argv[0], "--machine") == 0) {
			if ((C.name != NULL) || (argc < 2))
				usage();
			C.name = argv[1];
			argv++; argc--;
		} else if (strcmp(argv[0], "--keyfile") == 0) {
			if ((keyfilename != NULL) || (argc < 2))
				usage();
			keyfilename = argv[1];
			argv++; argc--;
		} else if (strcmp(argv[0], "--oldkey") == 0) {
			if ((oldkeyfilename != NULL) || (argc < 2))
				usage();
			oldkeyfilename = argv[1];
			argv++; argc--;
		} else if (strcmp(argv[0], "--passphrase-mem") == 0) {
			if ((maxmem != 0) || (argc < 2))
				usage();
			if (humansize_parse(argv[1], &maxmem)) {
				warnp("Cannot parse --passphrase-mem"
				    " argument: %s", argv[1]);
				exit(1);
			}
			argv++; argc--;
		} else if (strcmp(argv[0], "--passphrased") == 0) {
			passphrased = 1;
		} else {
			usage();
		}
	}

	/*
	 * We must have a user name, machine name, key file, and old key
	 * file specified.
	 */
	if ((C.user == NULL) || (C.name == NULL) ||
	    (keyfilename == NULL) || (oldkeyfilename == NULL))
		usage();

	/*
	 * It doesn't make sense to specify --passphrase-mem if we're not
	 * using a passphrase.
	 */
	if ((maxmem != 0) && (passphrased == 0))
		usage();

	/* Sanity-check the user name. */
	if (strlen(C.user) > 255) {
		fprintf(stderr, "User name too long: %s\n", C.user);
		exit(1);
	}
	if (strlen(C.user) == 0) {
		fprintf(stderr, "User name must be non-empty\n");
		exit(1);
	}

	/* Sanity-check the machine name. */
	if (strlen(C.name) > 255) {
		fprintf(stderr, "Machine name too long: %s\n", C.name);
		exit(1);
	}
	if (strlen(C.name) == 0) {
		fprintf(stderr, "Machine name must be non-empty\n");
		exit(1);
	}

	/* Get a password. */
	if (tarsnap_readpass(&C.passwd, "Enter tarsnap account password",
	    NULL, 0)) {
		warnp("Error reading password");
		exit(1);
	}

	/*
	 * Create key file -- we do this now rather than later so that we
	 * avoid registering with the server if we won't be able to create
	 * the key file later.
	 */
	if ((keyfile = fopen(keyfilename, "a")) == NULL) {
		warnp("Cannot create %s", keyfilename);
		exit(1);
	}

	/* Lock the key file to safeguard against simultaneous keygen runs. */
#ifdef HAVE_LOCKF
	while (lockf(fileno(keyfile), F_LOCK, 0)) {
#else
	while (flock(fileno(keyfile), LOCK_EX)) {
#endif
		/* Retry on EINTR. */
		if (errno == EINTR)
			continue;

		warnp("Cannot lock file: %s", keyfilename);
		exit(1);
	}

	/*
	 * Make sure that the file is currently empty, i.e., that we're not
	 * writing to a preexisting key file.
	 */
	if (fstat(fileno(keyfile), &sb)) {
		warnp("fstat(%s)", keyfilename);
		exit(1);
	}
	if (sb.st_size > 0) {
		warn0("Key file already exists, not overwriting: %s",
		    keyfilename);
		exit(1);
	}

	/* Set the permissions on the key file to 0600. */
	if (fchmod(fileno(keyfile), S_IRUSR | S_IWUSR)) {
		warnp("Cannot set permissions on key file: %s", keyfilename);
		exit(1);
	}

	/* Initialize entropy subsystem. */
	if (crypto_entropy_init()) {
		warnp("Entropy subsystem initialization failed");
		exit(1);
	}

	/* Initialize key cache. */
	if (crypto_keys_init()) {
		warnp("Key cache initialization failed");
		exit(1);
	}

	/*
	 * Load the keys CRYPTO_KEY_HMAC_{CHUNK, NAME, CPARAMS} from the old
	 * key file, since these are the keys which need to be consistent in
	 * order for two key sets to be compatible.  (CHUNK and NAME are used
	 * to compute the 32-byte keys for blocks; CPARAMS is used to compute
	 * parameters used to split a stream of bytes into chunks.)
	 */
	if (keyfile_read(oldkeyfilename, &dummy,
	    CRYPTO_KEYMASK_HMAC_CHUNK |
	    CRYPTO_KEYMASK_HMAC_NAME |
	    CRYPTO_KEYMASK_HMAC_CPARAMS)) {
		warnp("Error reading old key file");
		exit(1);
	}

	/*
	 * Generate all of the user keys except for the ones which we loaded
	 * from the old keyfile.
	 */
	if (crypto_keys_generate(CRYPTO_KEYMASK_USER &
	    ~CRYPTO_KEYMASK_HMAC_CHUNK &
	    ~CRYPTO_KEYMASK_HMAC_NAME &
	    ~CRYPTO_KEYMASK_HMAC_CPARAMS)) {
		warnp("Error generating keys");
		exit(1);
	}

	/* Initialize network layer. */
	if (network_init()) {
		warnp("Network layer initialization failed");
		exit(1);
	}

	/*
	 * We're not done, haven't answered a challenge, and don't have a
	 * machine number.
	 */
	C.done = 0;
	C.donechallenge = 0;
	C.machinenum = (uint64_t)(-1);

	/* Open netpacket connection. */
	if ((NPC = netpacket_open(USERAGENT)) == NULL)
		goto err1;

	/* Ask the netpacket layer to send a request and get a response. */
	if (netpacket_op(NPC, callback_register_send, &C))
		goto err1;

	/* Run event loop until an error occurs or we're done. */
	if (network_spin(&C.done))
		goto err1;

	/* Close netpacket connection. */
	if (netpacket_close(NPC))
		goto err1;

	/*
	 * If we didn't respond to a challenge, the server's response must
	 * have been a "no such user" error.
	 */
	if ((C.donechallenge == 0) && (C.status != 1)) {
		netproto_printerr(NETPROTO_STATUS_PROTERR);
		exit(1);
	}

	/* The machine number should be -1 iff the status is nonzero. */
	if (((C.machinenum == (uint64_t)(-1)) && (C.status == 0)) ||
	    ((C.machinenum != (uint64_t)(-1)) && (C.status != 0))) {
		netproto_printerr(NETPROTO_STATUS_PROTERR);
		exit(1);
	}

	/* Parse status returned by server. */
	switch (C.status) {
	case 0:
		/* Success! */
		break;
	case 1:
		warn0("No such user: %s", C.user);
		break;
	case 2:
		warn0("Incorrect password");
		break;
	case 3:
		warn0("Cannot register with server: "
		    "Account balance for user %s is not positive", C.user);
		break;
	default:
		netproto_printerr(NETPROTO_STATUS_PROTERR);
		warnp("Error registering with server");
		exit(1);
	}

	/* Shut down the network event loop. */
	network_fini();

	/* Exit with a code of 1 if we couldn't register. */
	if (C.machinenum == (uint64_t)(-1))
		exit(1);

	/* If the user wants to passphrase the keyfile, get the passphrase. */
	if (passphrased != 0) {
		if (tarsnap_readpass(&passphrase,
		    "Please enter passphrase for keyfile encryption",
		    "Please confirm passphrase for keyfile encryption", 1)) {
			warnp("Error reading password");
			exit(1);
		}
	} else {
		passphrase = NULL;
	}

	/* Write keys to file. */
	if (keyfile_write_file(keyfile, C.machinenum,
	    CRYPTO_KEYMASK_USER, passphrase, maxmem, 1.0))
		exit(1);

	/* Close the key file. */
	if (fclose(keyfile)) {
		warnp("Error closing key file");
		exit(1);
	}

	/* Success! */
	return (0);

err1:
	warnp("Error registering with server");
	exit(1);
}

static int
callback_register_send(void * cookie, NETPACKET_CONNECTION * NPC)
{
	struct register_internal * C = cookie;

	/* Tell the server which user is trying to add a machine. */
	return (netpacket_register_request(NPC, C->user,
	    callback_register_challenge));
}

static int
callback_register_challenge(void * cookie, NETPACKET_CONNECTION * NPC,
    int status, uint8_t packettype, const uint8_t * packetbuf,
    size_t packetlen)
{
	struct register_internal * C = cookie;
	uint8_t pub[CRYPTO_DH_PUBLEN];
	uint8_t priv[CRYPTO_DH_PRIVLEN];
	uint8_t K[CRYPTO_DH_KEYLEN];
	uint8_t keys[96];

	/* Handle errors. */
	if (status != NETWORK_STATUS_OK) {
		netproto_printerr(status);
		goto err0;
	}

	/*
	 * Make sure we received the right type of packet.  It is legal for
	 * the server to send back a NETPACKET_REGISTER_RESPONSE at this
	 * point; call callback_register_response to handle those.
	 */
	if (packettype == NETPACKET_REGISTER_RESPONSE)
		return (callback_register_response(cookie, NPC, status,
		    packettype, packetbuf, packetlen));
	else if (packettype != NETPACKET_REGISTER_CHALLENGE) {
		netproto_printerr(NETPROTO_STATUS_PROTERR);
		goto err0;
	}

	/* Generate DH parameters from the password and salt. */
	if (crypto_passwd_to_dh(C->passwd, packetbuf, pub, priv)) {
		warnp("Could not generate DH parameter from password");
		goto err0;
	}

	/* Compute shared key. */
	if (crypto_dh_compute(&packetbuf[32], priv, K))
		goto err0;
	if (crypto_hash_data(CRYPTO_KEY_HMAC_SHA256, K, CRYPTO_DH_KEYLEN,
	    C->register_key)) {
		warn0("Programmer error: "
		    "SHA256 should never fail");
		goto err0;
	}

	/* Export access keys. */
	if (crypto_keys_raw_export_auth(keys))
		goto err0;

	/* Send challenge response packet. */
	if (netpacket_register_cha_response(NPC, keys, C->name,
	    C->register_key, callback_register_response))
		goto err0;

	/* We've responded to a challenge. */
	C->donechallenge = 1;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

static int
callback_register_response(void * cookie, NETPACKET_CONNECTION * NPC,
    int status, uint8_t packettype, const uint8_t * packetbuf,
    size_t packetlen)
{
	struct register_internal * C = cookie;
	uint8_t hmac_actual[32];

	(void)NPC; /* UNUSED */
	(void)packetlen; /* UNUSED */

	/* Handle errors. */
	if (status != NETWORK_STATUS_OK) {
		netproto_printerr(status);
		goto err0;
	}

	/* Make sure we received the right type of packet. */
	if (packettype != NETPACKET_REGISTER_RESPONSE)
		goto err1;

	/* Verify packet hmac. */
	if ((packetbuf[0] == 0) || (packetbuf[0] == 3)) {
		crypto_hash_data_key_2(C->register_key, 32, &packettype, 1,
		    packetbuf, 9, hmac_actual);
	} else {
		memset(hmac_actual, 0, 32);
	}
	if (crypto_verify_bytes(hmac_actual, &packetbuf[9], 32))
		goto err1;

	/* Record status code and machine number returned by server. */
	C->status = packetbuf[0];
	C->machinenum = be64dec(&packetbuf[1]);

	/* We have received a response. */
	C->done = 1;

	/* Success! */
	return (0);

err1:
	netproto_printerr(NETPROTO_STATUS_PROTERR);
err0:
	/* Failure! */
	return (-1);
}