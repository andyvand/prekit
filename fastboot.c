/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>

#include <sys/time.h>
#include "libzipfile/zipfile.h"

#include "fastboot.h"

#define FW_DNX_BIN      "dnx.bin"
#define IFWI_BIN        "ifwi.bin"
#define NORMALOS_BIN    "stitch.normalos.bin"
#define PREOS_BIN       "stitch.preos.bin"
#define PLATFORM_IMG    "platform.img.gz"

static usb_handle *usb = 0;
static const char *serial = 0;
static int wipe_data = 0;
static unsigned short vendor_id = 0;

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr,"error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr,"\n");
    va_end(ap);
    exit(1);
}

#ifdef _WIN32
void *load_file(const char *fn, unsigned *_sz);
#else
void *load_file(const char *fn, unsigned *_sz)
{
    char *data;
    int sz;
    int fd;
    int errno_tmp;

    data = 0;
    fd = open(fn, O_RDONLY);
    if(fd < 0) return 0;

    sz = lseek(fd, 0, SEEK_END);
    if(sz < 0) goto oops;

    if(lseek(fd, 0, SEEK_SET) != 0) goto oops;

    data = (char*) malloc(sz);
    if(data == 0) goto oops;

    if(read(fd, data, sz) != sz) goto oops;
    close(fd);

    if(_sz) *_sz = sz;
    return data;

oops:
    errno_tmp = errno;
    close(fd);
    if(data != 0) free(data);
    errno = errno_tmp;
    return 0;
}
#endif

int match_fastboot(usb_ifc_info *info)
{
    if(!(vendor_id && (info->dev_vendor == vendor_id)) &&
       (info->dev_vendor != 0x18d1) &&  // Google
       (info->dev_vendor != 0x0451) &&
       (info->dev_vendor != 0x0502) &&
       (info->dev_vendor != 0x0fce) &&  // Sony Ericsson
       (info->dev_vendor != 0x05c6) &&  // Qualcomm
       (info->dev_vendor != 0x22b8) &&  // Motorola
       (info->dev_vendor != 0x0955) &&  // Nvidia
       (info->dev_vendor != 0x413c) &&  // DELL
       (info->dev_vendor != 0x8087) &&  // Intel
       (info->dev_vendor != 0x0bb4))    // HTC
            return -1;
    if(info->ifc_class != 0xff) return -1;
    if(info->ifc_subclass != 0x42) return -1;
    if(info->ifc_protocol != 0x03) return -1;
    // require matching serial number if a serial number is specified
    // at the command line with the -s option.
    if (serial && strcmp(serial, info->serial_number) != 0) return -1;
    return 0;
}

int list_devices_callback(usb_ifc_info *info)
{
    if (match_fastboot(info) == 0) {
        char* serial = info->serial_number;
        if (!info->writable) {
            serial = "no permissions"; // like "adb devices"
        }
        if (!serial[0]) {
            serial = "????????????";
        }
        // output compatible with "adb devices"
        printf("%s\tfastboot\n", serial);
    }

    return -1;
}

usb_handle *open_device(void)
{
    static usb_handle *usb = 0;
    int announce = 1;

    if(usb) return usb;

    for(;;) {
        usb = usb_open(match_fastboot);
        if(usb) return usb;
        if(announce) {
            announce = 0;
            fprintf(stderr,"< waiting for device >\n");
        }
        sleep(1);
    }
}

void list_devices(void) {
    // We don't actually open a USB device here,
    // just getting our callback called so we can
    // list all the connected devices.
    usb_open(list_devices_callback);
}

void usage(void)
{
    fprintf(stderr,
/*           1234567890123456789012345678901234567890123456789012345678901234567890123456 */
            "usage: fastboot [ <option> ] <command>\n"
            "\n"
            "commands:\n"
            "  flashall <filename>                      reflash device from a zip package\n"
            "  flash <partition> <filename>             write a file to a flash partition\n"
            "  erase <partition>                        erase a flash partition\n"
            "  getvar <variable>                        display a bootloader variable\n"
            "  devices                                  list all connected devices\n"
            "  continue                                 continue with autoboot\n"
            "  reboot                                   reboot device normally\n"
            "  reboot-bootloader                        reboot device into bootloader\n"
            "  help                                     show this help message\n"
            "\n"
            "options:\n"
            "  -s <serial number>                       specify device serial number\n"
            "  -i <vendor id>                           specify a custom USB vendor id\n"
        );
}

void *unzip_file(zipfile_t zip, const char *name, unsigned *sz)
{
    void *data;
    zipentry_t entry;
    unsigned datasz;

    entry = lookup_zipentry(zip, name);
    if (entry == NULL) {
        fprintf(stderr, "archive does not contain '%s'\n", name);
        return 0;
    }

    *sz = get_zipentry_size(entry);

    datasz = *sz * 1.001;
    data = malloc(datasz);

    if(data == 0) {
        fprintf(stderr, "failed to allocate %d bytes\n", *sz);
        return 0;
    }

    if (decompress_zipentry(entry, data, datasz)) {
        fprintf(stderr, "failed to unzip '%s' from archive\n", name);
        free(data);
        return 0;
    }

    return data;
}

static char *strip(char *s)
{
    int n;
    while(*s && isspace(*s)) s++;
    n = strlen(s);
    while(n-- > 0) {
        if(!isspace(s[n])) break;
        s[n] = 0;
    }
    return s;
}

void queue_info_dump(void)
{
    fb_queue_notice("--------------------------------------------");
    fb_queue_display("preos", "Current Pre-OS Version ");
    fb_queue_display("ifwi",  "Current IFWI Version   ");
    fb_queue_notice("--------------------------------------------");
}

void do_flashall(char *fn)
{
    void *zdata;
    unsigned zsize;
    void *data;
    unsigned sz;
    zipfile_t zip;

    queue_info_dump();

    zdata = load_file(fn, &zsize);
    if (zdata == 0) die("failed to load '%s': %s", fn, strerror(errno));

    zip = init_zipfile(zdata, zsize);
    if(zip == 0) die("failed to access zipdata in '%s'");

    data = unzip_file(zip, FW_DNX_BIN, &sz);
    if (data == 0) die("package missing %s", FW_DNX_BIN);
    fb_queue_flash("dnx", data, sz);

    data = unzip_file(zip, IFWI_BIN, &sz);
    if (data == 0) die("package missing %s", IFWI_BIN);
    fb_queue_flash("ifwi", data, sz);

    data = unzip_file(zip, NORMALOS_BIN, &sz);
    if (data == 0) die("package missing %s", NORMALOS_BIN);
    fb_queue_flash("boot", data, sz);

    data = unzip_file(zip, PREOS_BIN, &sz);
    if (data == 0) die("package missing %s\n", PREOS_BIN);
    fb_queue_flash("preos", data, sz);

    data = unzip_file(zip, PLATFORM_IMG, &sz);
    if (data == 0) die("package missing %s\n", PLATFORM_IMG);
    fb_queue_flash("platform", data, sz);
}

void do_send_signature(char *fn)
{
    void *data;
    unsigned sz;
    char *xtn;

    xtn = strrchr(fn, '.');
    if (!xtn) return;
    if (strcmp(xtn, ".img")) return;

    strcpy(xtn,".sig");
    data = load_file(fn, &sz);
    strcpy(xtn,".img");
    if (data == 0) return;
    fb_queue_download("signature", data, sz);
    fb_queue_command("signature", "installing signature");
}

#define skip(n) do { argc -= (n); argv += (n); } while (0)
#define require(n) do { if (argc < (n)) {usage(); exit(1);}} while (0)

int do_oem_command(int argc, char **argv)
{
    int i;
    char command[256];
    if (argc <= 1) return 0;

    command[0] = 0;
    while(1) {
        strcat(command,*argv);
        skip(1);
        if(argc == 0) break;
        strcat(command," ");
    }

    fb_queue_command(command,"");
    return 0;
}

int main(int argc, char **argv)
{
    int wants_reboot = 0;
    int wants_reboot_bootloader = 0;
    void *data;
    unsigned sz;
    int status;

    skip(1);
    if (argc == 0) {
        usage();
        return 1;
    }

    if (!strcmp(*argv, "devices")) {
        list_devices();
        return 0;
    }

    if (!strcmp(*argv, "help")) {
        usage();
        return 0;
    }


    serial = getenv("ANDROID_SERIAL");

    while (argc > 0) {
        if(!strcmp(*argv, "-s")) {
            require(2);
            serial = argv[1];
            skip(2);
        } else if(!strcmp(*argv, "-i")) {
            char *endptr = NULL;
            unsigned long val;
            require(2);
            val = strtoul(argv[1], &endptr, 0);
            if (!endptr || *endptr != '\0' || (val & ~0xffff))
                die("invalid vendor id '%s'", argv[1]);
            vendor_id = (unsigned short)val;
            skip(2);
        } else if(!strcmp(*argv, "getvar")) {
            require(2);
            fb_queue_display(argv[1], argv[1]);
            skip(2);
        } else if(!strcmp(*argv, "erase")) {
            require(2);
            fb_queue_erase(argv[1]);
            skip(2);
        } else if(!strcmp(*argv, "signature")) {
            require(2);
            data = load_file(argv[1], &sz);
            if (data == 0) die("could not load '%s': %s", argv[1], strerror(errno));
            if (sz != 256) die("signature must be 256 bytes");
            fb_queue_download("signature", data, sz);
            fb_queue_command("signature", "installing signature");
            skip(2);
        } else if(!strcmp(*argv, "reboot")) {
            wants_reboot = 1;
            skip(1);
        } else if(!strcmp(*argv, "reboot-bootloader")) {
            wants_reboot_bootloader = 1;
            skip(1);
        } else if (!strcmp(*argv, "continue")) {
            fb_queue_command("continue", "resuming boot");
            skip(1);
        } else if(!strcmp(*argv, "flash")) {
            char *pname = argv[1];
            char *fname = 0;
            require(3);
            fname = argv[2];
            skip(3);
            data = load_file(fname, &sz);
            if (data == 0) die("cannot load '%s': %s\n", fname, strerror(errno));
            fb_queue_flash(pname, data, sz);
        } else if(!strcmp(*argv, "flashall")) {
            require(2);
            do_flashall(argv[1]);
            skip(2);
            wants_reboot = 1;
        } else if(!strcmp(*argv, "oem")) {
            argc = do_oem_command(argc, argv);
        } else {
            usage();
            return 1;
        }
    }

    if (wants_reboot) {
        fb_queue_reboot();
    } else if (wants_reboot_bootloader) {
        fb_queue_command("reboot-bootloader", "rebooting into bootloader");
    }

    usb = open_device();

    status = fb_execute_queue(usb);
    return (status) ? 1 : 0;
}
