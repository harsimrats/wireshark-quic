/* eyesdn.c
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#include "wtap-int.h"
#include "eyesdn.h"
#include "file_wrappers.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int eyesdn_file_type_subtype = -1;

void register_eyesdn(void);

/* This module reads the output of the EyeSDN USB S0/E1 ISDN probes
 * They store HDLC frames of D and B channels in a binary format
 * The fileformat is
 *
 * 1-6 Byte: EyeSDN - Magic
 * 7-n Byte: Frames
 *
 * Each Frame starts with the 0xff Flag byte
 * - Bytes 0-2: timestamp (usec in network byte order)
 * - Bytes 3-7: timestamp (40bits sec since 1970 in network byte order)
 * - Byte 8: channel (0 for D channel, 1-30 for B1-B30)
 * - Byte 9: Sender Bit 0(0 NT, 1 TE), Protocol in Bits 7:1, see enum
 * - Byte 10-11: frame size in bytes
 * - Byte 12-n: Frame Payload
 *
 * All multibyte values are represented in network byte order
 * The frame is terminated with a flag character (0xff)
 * bytes 0xff within a frame are escaped using the 0xfe escape character
 * the byte following the escape character is decremented by two:
 * so 0xfe 0xfd is actually a 0xff
 * Characters that need to be escaped are 0xff and 0xfe
 */


static gboolean esc_read(FILE_T fh, guint8 *buf, int len, int *err, gchar **err_info)
{
	int i;
	int value;

	for(i=0; i<len; i++) {
		value=file_getc(fh);
		if(value==-1) {
			/* EOF or error */
			*err=file_error(fh, err_info);
			if(*err==0)
				*err=WTAP_ERR_SHORT_READ;
			return FALSE;
		}
		if(value==0xff) {
			/* error !!, read into next frame */
			*err=WTAP_ERR_BAD_FILE;
			*err_info=g_strdup("eyesdn: No flag character seen in frame");
			return FALSE;
		}
		if(value==0xfe) {
			/* we need to escape */
			value=file_getc(fh);
			if(value==-1) {
				/* EOF or error */
				*err=file_error(fh, err_info);
				if(*err==0)
					*err=WTAP_ERR_SHORT_READ;
				return FALSE;
			}
			value+=2;
		}
		buf[i]=value;
	}

	return TRUE;
}

/* Magic text to check for eyesdn-ness of file */
static const unsigned char eyesdn_hdr_magic[]  =
{ 'E', 'y', 'e', 'S', 'D', 'N'};
#define EYESDN_HDR_MAGIC_SIZE  sizeof(eyesdn_hdr_magic)

/* Size of a record header */
#define EYESDN_HDR_LENGTH		12

static gboolean eyesdn_read(wtap *wth, wtap_rec *rec, Buffer *buf,
	int *err, gchar **err_info, gint64 *data_offset);
static gboolean eyesdn_seek_read(wtap *wth, gint64 seek_off,
	wtap_rec *rec, Buffer *buf, int *err, gchar **err_info);
static int read_eyesdn_rec(FILE_T fh, wtap_rec *rec, Buffer* buf,
	int *err, gchar **err_info);

/* Seeks to the beginning of the next packet, and returns the
   byte offset.  Returns -1 on failure, and sets "*err" to the error
   and "*err_info" to null or an additional error string. */
static gint64 eyesdn_seek_next_packet(wtap *wth, int *err, gchar **err_info)
{
	int byte;
	gint64 cur_off;

	while ((byte = file_getc(wth->fh)) != EOF) {
		if (byte == 0xff) {
			cur_off = file_tell(wth->fh);
			if (cur_off == -1) {
				/* Error. */
				*err = file_error(wth->fh, err_info);
				return -1;
			}
			return cur_off;
		}
	}
	/* EOF or error. */
	*err = file_error(wth->fh, err_info);
	return -1;
}

wtap_open_return_val eyesdn_open(wtap *wth, int *err, gchar **err_info)
{
	char	magic[EYESDN_HDR_MAGIC_SIZE];

	/* Look for eyesdn header */
	if (!wtap_read_bytes(wth->fh, &magic, sizeof magic, err, err_info)) {
		if (*err != WTAP_ERR_SHORT_READ)
			return WTAP_OPEN_ERROR;
		return WTAP_OPEN_NOT_MINE;
	}
	if (memcmp(magic, eyesdn_hdr_magic, EYESDN_HDR_MAGIC_SIZE) != 0)
		return WTAP_OPEN_NOT_MINE;

	wth->file_encap = WTAP_ENCAP_PER_PACKET;
	wth->file_type_subtype = eyesdn_file_type_subtype;
	wth->snapshot_length = 0; /* not known */
	wth->subtype_read = eyesdn_read;
	wth->subtype_seek_read = eyesdn_seek_read;
	wth->file_tsprec = WTAP_TSPREC_USEC;

	return WTAP_OPEN_MINE;
}

/* Find the next record and parse it; called from wtap_read(). */
static gboolean eyesdn_read(wtap *wth, wtap_rec *rec, Buffer *buf,
    int *err, gchar **err_info, gint64 *data_offset)
{
	gint64	offset;

	/* Find the next record */
	offset = eyesdn_seek_next_packet(wth, err, err_info);
	if (offset < 1)
		return FALSE;
	*data_offset = offset;

	/* Parse the record */
	return read_eyesdn_rec(wth->fh, rec, buf, err, err_info);
}

/* Used to read packets in random-access fashion */
static gboolean
eyesdn_seek_read(wtap *wth, gint64 seek_off, wtap_rec *rec,
	Buffer *buf, int *err, gchar **err_info)
{
	if (file_seek(wth->random_fh, seek_off, SEEK_SET, err) == -1)
		return FALSE;

	return read_eyesdn_rec(wth->random_fh, rec, buf, err, err_info);
}

/* Parses a record. */
static gboolean
read_eyesdn_rec(FILE_T fh, wtap_rec *rec, Buffer *buf, int *err,
    gchar **err_info)
{
	union wtap_pseudo_header *pseudo_header = &rec->rec_header.packet_header.pseudo_header;
	guint8		hdr[EYESDN_HDR_LENGTH];
	time_t		secs;
	int		usecs;
	guint		pkt_len;
	guint8		channel, direction;
	guint8		*pd;

	/* Our file pointer should be at the summary information header
	 * for a packet. Read in that header and extract the useful
	 * information.
	 */
	if (!esc_read(fh, hdr, EYESDN_HDR_LENGTH, err, err_info))
		return FALSE;

	/* extract information from header */
	usecs = pntoh24(&hdr[0]);
#ifdef TV64BITS
	secs = hdr[3];
#else
	secs = 0;
#endif
	secs = (secs << 8) | hdr[4];
	secs = (secs << 8) | hdr[5];
	secs = (secs << 8) | hdr[6];
	secs = (secs << 8) | hdr[7];

	channel = hdr[8];
	direction = hdr[9];
	pkt_len = pntoh16(&hdr[10]);

	switch(direction >> 1) {

	default:
	case EYESDN_ENCAP_ISDN: /* ISDN */
		pseudo_header->isdn.uton = direction & 1;
		pseudo_header->isdn.channel = channel;
		if(channel) { /* bearer channels */
			rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_ISDN; /* recognises PPP */
			pseudo_header->isdn.uton=!pseudo_header->isdn.uton; /* bug */
		} else { /* D channel */
			rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_ISDN;
		}
		break;

	case EYESDN_ENCAP_MSG: /* Layer 1 message */
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_LAYER1_EVENT;
		pseudo_header->l1event.uton = (direction & 1);
		break;

	case EYESDN_ENCAP_LAPB: /* X.25 via LAPB */
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_LAPB;
		pseudo_header->dte_dce.flags = (direction & 1) ? 0 : 0x80;
		break;

	case EYESDN_ENCAP_ATM: { /* ATM cells */
#define CELL_LEN 53
		unsigned char cell[CELL_LEN];
		gint64 cur_off;

		if(pkt_len != CELL_LEN) {
			*err = WTAP_ERR_BAD_FILE;
			*err_info = ws_strdup_printf(
			    "eyesdn: ATM cell has a length != 53 (%u)",
			    pkt_len);
			return FALSE;
		}

		cur_off = file_tell(fh);
		if (!esc_read(fh, cell, CELL_LEN, err, err_info))
			return FALSE;
		if (file_seek(fh, cur_off, SEEK_SET, err) == -1)
			return FALSE;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_ATM_PDUS_UNTRUNCATED;
		pseudo_header->atm.flags=ATM_RAW_CELL;
		pseudo_header->atm.aal=AAL_UNKNOWN;
		pseudo_header->atm.type=TRAF_UMTS_FP;
		pseudo_header->atm.subtype=TRAF_ST_UNKNOWN;
		pseudo_header->atm.vpi=((cell[0]&0xf)<<4) + (cell[0]&0xf);
		pseudo_header->atm.vci=((cell[0]&0xf)<<4) + cell[0]; /* from cell */
		pseudo_header->atm.channel=direction & 1;
		}
		break;

	case EYESDN_ENCAP_MTP2: /* SS7 frames */
		pseudo_header->mtp2.sent = direction & 1;
		pseudo_header->mtp2.annex_a_used = MTP2_ANNEX_A_USED_UNKNOWN;
		pseudo_header->mtp2.link_number = channel;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_MTP2_WITH_PHDR;
		break;

	case EYESDN_ENCAP_DPNSS: /* DPNSS */
		pseudo_header->isdn.uton = direction & 1;
		pseudo_header->isdn.channel = channel;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_DPNSS;
		break;

	case EYESDN_ENCAP_DASS2: /* DASS2 frames */
		pseudo_header->isdn.uton = direction & 1;
		pseudo_header->isdn.channel = channel;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_DPNSS;
		break;

	case EYESDN_ENCAP_BACNET: /* BACNET async over HDLC frames */
	        pseudo_header->isdn.uton = direction & 1;
		pseudo_header->isdn.channel = channel;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_BACNET_MS_TP_WITH_PHDR;
		break;

	case EYESDN_ENCAP_V5_EF: /* V5EF */
		pseudo_header->isdn.uton = direction & 1;
		pseudo_header->isdn.channel = channel;
		rec->rec_header.packet_header.pkt_encap = WTAP_ENCAP_V5_EF;
		break;
	}

	if(pkt_len > WTAP_MAX_PACKET_SIZE_STANDARD) {
		*err = WTAP_ERR_BAD_FILE;
		*err_info = ws_strdup_printf("eyesdn: File has %u-byte packet, bigger than maximum of %u",
		    pkt_len, WTAP_MAX_PACKET_SIZE_STANDARD);
		return FALSE;
	}

	rec->rec_type = REC_TYPE_PACKET;
	rec->block = wtap_block_create(WTAP_BLOCK_PACKET);
	rec->presence_flags = WTAP_HAS_TS;
	rec->ts.secs = secs;
	rec->ts.nsecs = usecs * 1000;
	rec->rec_header.packet_header.caplen = pkt_len;
	rec->rec_header.packet_header.len = pkt_len;

	/* Make sure we have enough room for the packet */
	ws_buffer_assure_space(buf, pkt_len);

	pd = ws_buffer_start_ptr(buf);
	if (!esc_read(fh, pd, pkt_len, err, err_info))
		return FALSE;
	return TRUE;
}


static gboolean
esc_write(wtap_dumper *wdh, const guint8 *buf, int len, int *err)
{
	int i;
	guint8 byte;
	static const guint8 esc = 0xfe;

	for(i=0; i<len; i++) {
		byte=buf[i];
		if(byte == 0xff || byte == 0xfe) {
			/*
			 * Escape the frame delimiter and escape byte.
			 */
			if (!wtap_dump_file_write(wdh, &esc, sizeof esc, err))
				return FALSE;
			byte-=2;
		}
		if (!wtap_dump_file_write(wdh, &byte, sizeof byte, err))
			return FALSE;
	}
	return TRUE;
}

static gboolean eyesdn_dump(wtap_dumper *wdh,
			    const wtap_rec *rec,
			    const guint8 *pd, int *err, gchar **err_info);

static gboolean eyesdn_dump_open(wtap_dumper *wdh, int *err, gchar **err_info _U_)
{
	wdh->subtype_write=eyesdn_dump;

	if (!wtap_dump_file_write(wdh, eyesdn_hdr_magic,
	    EYESDN_HDR_MAGIC_SIZE, err))
		return FALSE;
	*err=0;
	return TRUE;
}

static int eyesdn_dump_can_write_encap(int encap)
{
	switch (encap) {
	case WTAP_ENCAP_ISDN:
	case WTAP_ENCAP_LAYER1_EVENT:
	case WTAP_ENCAP_DPNSS:
	case WTAP_ENCAP_ATM_PDUS_UNTRUNCATED:
	case WTAP_ENCAP_LAPB:
	case WTAP_ENCAP_MTP2_WITH_PHDR:
	case WTAP_ENCAP_BACNET_MS_TP_WITH_PHDR:
	case WTAP_ENCAP_PER_PACKET:
		return 0;

	default:
		return WTAP_ERR_UNWRITABLE_ENCAP;
	}
}

/* Write a record for a packet to a dump file.
 *    Returns TRUE on success, FALSE on failure. */
static gboolean eyesdn_dump(wtap_dumper *wdh,
			    const wtap_rec *rec,
			    const guint8 *pd, int *err, gchar **err_info _U_)
{
	static const guint8 start_flag = 0xff;
	const union wtap_pseudo_header *pseudo_header = &rec->rec_header.packet_header.pseudo_header;
	guint8 buf[EYESDN_HDR_LENGTH];
	int usecs;
	time_t secs;
	int channel;
	int origin;
	int protocol;
	int size;

	/* We can only write packet records. */
	if (rec->rec_type != REC_TYPE_PACKET) {
		*err = WTAP_ERR_UNWRITABLE_REC_TYPE;
		return FALSE;
	}

	/* Don't write out anything bigger than we can read.
	 * (The length field in packet headers is 16 bits, which
	 * imposes a hard limit.) */
	if (rec->rec_header.packet_header.caplen > 65535) {
		*err = WTAP_ERR_PACKET_TOO_LARGE;
		return FALSE;
	}

	usecs=rec->ts.nsecs/1000;
	secs=rec->ts.secs;
	size=rec->rec_header.packet_header.caplen;
	origin = pseudo_header->isdn.uton;
	channel = pseudo_header->isdn.channel;

	switch(rec->rec_header.packet_header.pkt_encap) {

	case WTAP_ENCAP_ISDN:
		protocol=EYESDN_ENCAP_ISDN; /* set depending on decoder format and mode */
		break;

	case WTAP_ENCAP_LAYER1_EVENT:
		protocol=EYESDN_ENCAP_MSG;
		break;

	case WTAP_ENCAP_DPNSS:
		protocol=EYESDN_ENCAP_DPNSS;
		break;

#if 0
	case WTAP_ENCAP_DASS2:
		protocol=EYESDN_ENCAP_DASS2;
		break;
#endif

	case WTAP_ENCAP_ATM_PDUS_UNTRUNCATED:
		protocol=EYESDN_ENCAP_ATM;
		channel=0x80;
		break;

	case WTAP_ENCAP_LAPB:
		protocol=EYESDN_ENCAP_LAPB;
		break;

	case WTAP_ENCAP_MTP2_WITH_PHDR:
		protocol=EYESDN_ENCAP_MTP2;
		break;

	case WTAP_ENCAP_BACNET_MS_TP_WITH_PHDR:
		protocol=EYESDN_ENCAP_BACNET;
		break;

	case WTAP_ENCAP_V5_EF:
		protocol=EYESDN_ENCAP_V5_EF;
		break;

	default:
		*err=WTAP_ERR_UNWRITABLE_ENCAP;
		return FALSE;
	}

	phton24(&buf[0], usecs);

	buf[3] = (guint8)0;
	buf[4] = (guint8)(0xff & (secs >> 24));
	buf[5] = (guint8)(0xff & (secs >> 16));
	buf[6] = (guint8)(0xff & (secs >> 8));
	buf[7] = (guint8)(0xff & (secs >> 0));

	buf[8] = (guint8) channel;
	buf[9] = (guint8) (origin?1:0) + (protocol << 1);
	phtons(&buf[10], size);

	/* start flag */
	if (!wtap_dump_file_write(wdh, &start_flag, sizeof start_flag, err))
		return FALSE;
	if (!esc_write(wdh, buf, 12, err))
		return FALSE;
	if (!esc_write(wdh, pd, size, err))
		return FALSE;
	return TRUE;
}

static const struct supported_block_type eyesdn_blocks_supported[] = {
	/*
	 * We support packet blocks, with no comments or other options.
	 */
	{ WTAP_BLOCK_PACKET, MULTIPLE_BLOCKS_SUPPORTED, NO_OPTIONS_SUPPORTED }
};

static const struct file_type_subtype_info eyesdn_info = {
	"EyeSDN USB S0/E1 ISDN trace format", "eyesdn", "trc", NULL,
	FALSE, BLOCKS_SUPPORTED(eyesdn_blocks_supported),
	eyesdn_dump_can_write_encap, eyesdn_dump_open, NULL
};

void register_eyesdn(void)
{
	eyesdn_file_type_subtype = wtap_register_file_type_subtype(&eyesdn_info);

	/*
	 * Register name for backwards compatibility with the
	 * wtap_filetypes table in Lua.
	 */
	wtap_register_backwards_compatibility_lua_name("EYESDN",
	    eyesdn_file_type_subtype);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
