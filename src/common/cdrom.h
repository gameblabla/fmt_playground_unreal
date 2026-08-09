#ifndef FMT_CDROM_H
#define FMT_CDROM_H

#include <stdint.h>

/*
 * FM TOWNS CD-ROM sector reader for the *internal* CD-ROM controller
 * ("CDC"), addressed through I/O ports 0x4C0-0x4C8 (TOWNSIO_CDROM_*).
 *
 * This project boots via TOWNSEMU's "-CD image.iso" option (see
 * run.sh), which attaches the disc image to the internal CD-ROM device
 * (TownsCDROM/cdImgFName in TOWNSEMU/src/main_cui/argv/townsargv.cpp) -
 * a different, separate device from the general SCSI bus (which
 * FM/TOWNS/SCSILIB + FM/TOWNS/YSSCSICD's real DOS driver source talks
 * to, and which TOWNSEMU only populates via its debugger's SCSICDxLOAD
 * command, not by -CD). An initial SCSI-bus-based implementation here
 * was tested against this project's actual boot setup and never found
 * a device - IOMON showed the SCSI status port permanently reporting
 * "not busy", confirming nothing is listening on that bus in this
 * configuration. See scsi.[ch] for that driver, kept for a real
 * external/desktop-FM-TOWNS SCSI CD-ROM setup, but *not* exercised or
 * verified by this project's boot path - fmt_cdrom_read() here is the
 * one actually confirmed working (see below).
 *
 * Port map and CPU/PIO transfer command protocol reverse-engineered
 * from TOWNSEMU's CDC model (TOWNSEMU/src/towns/cdrom/cdrom.cpp,
 * TownsCDROM::IOWriteByte/IOReadByte/ExecuteCDROMCommand/
 * DelayedCommandExecution), which implements the FM TOWNS Technical
 * Databook pp.224-227 CDC command set:
 *
 *   0x4C0 Master control/status (read: DRY/STSF/DTSF/SIRQ/DEI bits)
 *   0x4C2 Command/status FIFO   (write: command byte; also status readback)
 *   0x4C4 Parameter/data        (write: command parameters; read: PIO
 *                                sector data once STSF is set)
 *   0x4C6 Transfer control      (write: 0x08 arms one sector of CPU/PIO
 *                                transfer, setting STSF once the CDC has
 *                                it ready)
 *
 * Protocol for a Mode 1 (2048 byte/sector) read of `count` sectors
 * starting at LBA `lba`, confirmed working end-to-end (booted, loaded
 * IMAGE.RAW+PALETTE.BIN via iso9660.c, rendered pixel-correct in
 * TOWNSEMU - see the fmt_iso9660_load() call site in main.c):
 *   1. Write the command byte (CDCMD_MODE1READ) to 0x4C2. This just
 *      latches state.cmd - execution doesn't start yet.
 *   2. Write 8 parameter bytes to 0x4C4: BCD-encoded start MSF (3 bytes),
 *      BCD-encoded end MSF (3 bytes, exclusive - start+count sectors),
 *      then 2 unused/zero filler bytes to reach the CDC's fixed 8-byte
 *      parameter queue (PARAM_QUEUE_LEN in cdrom.cpp) - the 8th byte is
 *      what actually triggers command execution.
 *   3. Per sector: write 0x08 to 0x4C6 to arm CPU-transfer mode for that
 *      sector, poll 0x4C0 until the STSF bit (0x20) is set, then read
 *      2048 bytes one at a time from 0x4C4. The CDC clears STSF and
 *      CPUTransfer after each sector, so this arm-poll-read cycle repeats
 *      once per sector rather than once per whole request.
 *
 * MSF (minute:second:frame) addressing follows Red Book convention:
 * frame 0 of track 1's data starts at MSF 00:02:00 (150-frame/2-second
 * pregap), so LBA and MSF convert via lba = M*60*75+S*75+F-150.
 */

/* Reads `count` Mode-1 2048-byte sectors starting at LBA `lba` into
 * `buf` (must hold count*2048 bytes). Returns 0 on success, -1 if the
 * CDC never became ready (timeout - e.g. no disc). Blocking. */
int fmt_cdrom_read(uint32_t lba, uint16_t count, void *buf);

/*----------------------------------------------------------------------
 * Streaming reader
 *
 * fmt_cdrom_read() above blocks for the whole transfer, which is fine
 * when nothing else is going on but useless for playing audio off the
 * disc: the CD handshake takes milliseconds per sector, and any
 * millisecond not spent feeding the DAC is an audible hole. This is the
 * same protocol driven as a state machine that never waits for
 * anything - every call does a bounded amount of I/O and returns,
 * whatever the drive is or isn't ready for - so it can be advanced from
 * inside a sample-paced playback loop (see pcmstream.c) using time that
 * loop would otherwise spend idling on the YM2612 busy flag.
 *
 * It fills a caller-owned ring buffer, whose size must be a power of
 * two and a multiple of 2048, at least 2*FMT_CD_STREAM_RUN_SECTORS
 * sectors so a run can always be started while the previous data is
 * still being consumed. The caller owns the read cursor and passes it
 * to each step; the machine owns the write cursor (`fill_pos`). Both
 * count bytes since the start and only ever increase, so the number of
 * bytes available is `fill_pos - play_pos` and the byte at `play_pos`
 * lives at `ring[play_pos & ring_mask]`.
 *--------------------------------------------------------------------*/

/* Sectors per MODE1READ command. Bigger amortises the command handshake
 * over more data; the ring has to be able to hold a whole run twice
 * over, and the run must be drainable well inside the drive's ~100ms
 * lost-data timeout. */
#define FMT_CD_STREAM_RUN_SECTORS   8u

enum {
    FMT_CD_STREAM_IDLE,   /* between commands - will start a run when the ring has room */
    FMT_CD_STREAM_ISSUE,  /* writing out the 9 command/parameter bytes */
    FMT_CD_STREAM_WAIT,   /* command in flight, waiting on the status FIFO */
    FMT_CD_STREAM_XFER,   /* a sector is ready, draining it into the ring */
    FMT_CD_STREAM_DONE,   /* end of a non-looping file */
    FMT_CD_STREAM_ERROR   /* the drive reported a failure */
};

typedef struct {
    uint8_t *ring;
    uint32_t ring_mask;      /* ring size - 1 */
    uint32_t fill_pos;       /* bytes deposited since the start */
    uint32_t lba;            /* first sector of the file */
    uint32_t size;           /* file length in bytes */
    uint32_t total_sectors;  /* sectors the file occupies */
    uint32_t sector;         /* next sector of the file to fetch */
    uint16_t byte_idx;       /* bytes taken so far from the sector in flight */
    uint8_t  cmd[9];         /* command byte + 8 parameter bytes being issued */
    uint8_t  cmd_idx;
    uint8_t  state;
    uint8_t  loop;
} fmt_cd_stream;

/* Prepares `st` to stream `size` bytes starting at LBA `lba` into
 * `ring`. With `loop` set, reaching the end of the file wraps back to
 * its start instead of finishing. Issues no I/O beyond clearing any
 * leftover drive status. */
void fmt_cdrom_stream_init(fmt_cd_stream *st, uint32_t lba, uint32_t size,
                           uint8_t *ring, uint32_t ring_size, int loop);

/* Advances the transfer by at most `budget` I/O operations and returns
 * how many payload bytes that added to the ring (0 whenever the step
 * went into command issue, status polling, or found the ring full).
 * `play_pos` is the caller's current read cursor. Never blocks; call it
 * as often as there is time for. */
unsigned fmt_cdrom_stream_step(fmt_cd_stream *st, uint32_t play_pos,
                               unsigned budget);

#endif
