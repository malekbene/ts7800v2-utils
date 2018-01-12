#define _GNU_SOURCE         /* See feature_test_macros(7) */
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <signal.h>


/**
   This is mostly a stub because as of 8/24/2017, the SiLabs code on the
   TS-7800-V2 doesn't have the ADC support, sleep, WDT.

*/

/* To compile ts7800ctl, use the appropriate cross compiler and run the
 * command:
 *
 *   gcc -Wall -O -mcpu=arm9 -o ts7800ctl ts7800ctl.c
 *
 * On uclibc based initrd's, the following additional gcc options are
 * necessary: -Wl,--rpath,/slib -Wl,-dynamic-linker,/slib/ld-uClibc.so.0
 */


#include "i2c-dev.h"

/*
   0,1   P1.2, CPU_CORE
   2,3   P1.3, RAM_1.35V
   4,5   P1.4, 1.2V
         P1.5, 1.8V
   6,7   P2.0, VIN (scaled 5.3%)
   8,9   P2.1  5.2V_A (scaled 44%)
   10,11 P2.2  AN_3.3V (scaled 50%)
   12,13 P2.3  ADC_4 (scaled 50%)
   14,17 reserved
   18,19 P2.4  ADC_3 (scaled 50%)
   20,21 P2.5  ADC_2 (scaled 50%)
   22,23 P2.6  ADC_1 (scaled 50%)
   24,25 P2.7  ADC_0 (scaled 50%)

*/

#define SILABS_CHIP_ADDRESS 0x54

volatile unsigned int *data, *control, *status, *led, *syscon, *cpuregs;
static unsigned int verbose, addr;
static int model, done, twifd;

static int get_model(void);
static int silabs_init(void);
static int silabs_read(int twifd, uint8_t *data, uint16_t addr, int bytes);
static int silabs_write(int twifd, uint8_t *data, uint16_t addr, int bytes);
static int parseMacAddress(const char *str, unsigned char *buf);
static inline void feed_wdt(int);
static inline void disable_wdt(void);
static inline void enable_wdt(void);
static inline void do_silabs_sleep(unsigned int);

static void exit_gracefully(int signum) {
   fprintf(stderr, "EXITING....\n");
   signal(signum, exit_gracefully);
   done = 1;
}


static void usage(char **);
static int parsechans(const char*, unsigned int);



static void usage(char **argv)
{
   fprintf(stderr, "Usage: %s [OPTION] ...\n"
     "Examine/Modify state of TS-7800-V2 hardware.\n"
     "\n"
     "General options:\n"
     "  -s    seconds         Number of seconds to sleep for\n"
     "  -f                    Feed the WDT for 8s\n"
     "  -d                    Disable the WDT\n"
     "  -r    CHANS           Sample ADC channels CHANS, e.g. \"0-2,4\", \"1,3,4\""
                                 "output raw data to standard out\n"
     "  -S    CHANS           Sample ADC channels CHANS, e.g. \"0-2,4\", \"1,3,4\""
                                  "output string parseable data to standard out\n"
     "  -n                    Red LED on\n"
     "  -F                    Red LED off\n"
     "  -g                    Green LED on\n"
     "  -G                    Green LED off\n"
     "  -i                    Display FPGA info\n"
     "  -o                    Display one time programmable data\n"
     "  -m                    Display contents of non-volatile memory\n"
     "  -A    ADDR            Write DATA to ADDR in non-volatile memory\n"
     "  -D    DATA            Write DATA to ADDR in non-volatile memory\n"
     "  -M[xx:xx:xx:xx:xx:xx] Display [or optionally set] the MAC address\n"
     "  -O                    Display odometer(hrs board has been running for)\n"
     "  -B                    Display birthdate\n"
     "  -V                    Verbose output\n"
     "  -h                    This help screen\n",program_invocation_short_name);
}


int main(int argc, char **argv)
{
   int c, devmem;
   unsigned int val_addr=0, val_data=0;
   int nvram_addr, secs = 0;
   int red_led_on=0, red_led_off=0, green_led_on=0, green_led_off=0;
   unsigned int display_otp=0, display_mem=0, display_mac=0, set_mac=0 ;
   unsigned int display_odom=0, did_something=0, display_bday=0;
   unsigned int start_adc=0, raw=0, do_info=0;
   unsigned int len = 0; //, odom, bday;
   char str[80];
   unsigned char new_mac[6], nvram_data = 0;;

   if(argc == 1) {
      usage(argv);
      return 1;
   }

   if ((model=get_model())) {
      if (model != 0x7800) {
         fprintf(stderr, "Unsupported model\n");
         return 1;
      }
   }
   else {
      fprintf(stderr, "model=unknown\n");
      return 1;
   }
   twifd = silabs_init();
   if(twifd == -1) {
     fprintf(stderr, "ERROR: Cannot initialize connection to Silabs via /dev/i2c-0\n");
     return 1;
   }

   signal(SIGTERM, exit_gracefully);
   signal(SIGHUP, exit_gracefully);
   signal(SIGINT, exit_gracefully);
   signal(SIGPIPE, exit_gracefully);

   nvram_addr = nvram_data = -1;

   while ((c = getopt(argc, argv, "s:fdr:S:A:D:inFgGVoOmM::B")) != -1) {
      switch(c) {
         case 's':

            secs = strtoul(optarg, NULL, 0);
            if((secs > 0) && (secs <= 524288)){
               did_something=1;
            } else {
               printf("Invalid sleep time,"
                 "maximum sleep time is 524288\n");
               secs = 0;
            }
            break;

         case 'f':
            feed_wdt(8);
            break;

         case 'i':
            do_info = 1;
            break;

         case 'd':
            disable_wdt();
            break;

         case 'r':
            start_adc=1;
            raw=1;
            for(len=0;len<(sizeof(str)-1);len++)
               if(optarg[len]=='\0') break;
            strncpy(str, optarg, len);
            str[len] = '\0';
            break;

         case 'S':
            start_adc=1;
            for(len=0;len<80;len++)
               if(optarg[len]=='\0') break;
            strncpy(str, optarg, len);
            str[len] = '\0';			   break;

         case 'A':
            nvram_addr = strtoul(optarg, NULL, 0);
            if(nvram_addr >= 0 && nvram_addr < 16) val_addr = 1;
            else { fprintf(stderr, "Invalid address,"
                     " valid address are 0-15\n");
               nvram_addr = -1;
            }
            break;

         case 'D':
            nvram_data = strtoul(optarg, NULL, 0);
            if(nvram_data >= 0 && nvram_data < 256) val_data = 1;
            else {
               fprintf(stderr, "Invalid data,"
                  " valid data is 0-255\n");
               nvram_data = -1;
            }
            break;

         case 'n':
            red_led_on = 1;
            break;

         case 'F':
            red_led_off = 1;
            break;

         case 'g':
            green_led_on = 1;
            break;

         case 'G':
            green_led_off = 1;
            break;

         case 'V':
            verbose = 1;
            break;

         case 'M':
            if (optarg) {
               if (! parseMacAddress(optarg, new_mac)) {
                  fprintf(stderr, "Invalid MAC: %s\n", optarg);
                  return 1;
               }
               set_mac = 1;
            }
            display_mac = 1;
            did_something=1;
            break;

         case 'O':
            display_odom = 1;
            did_something=1;
            break;

         case 'B':
            display_bday = 1;
            did_something=1;
            break;

         case 'o':
            display_otp = 1;
            did_something=1;
            addr=6;
            break;

         case 'm':
            display_mem = 1;
            did_something=1;
            addr=1536;
            break;

         case 'h':
         default:
            usage(argv);
            return 1;
      }
   }

   if (red_led_on && red_led_off) {
      fprintf(stderr, "red LED on and off are mutually exclusive\n");
   } else if (red_led_on || red_led_off) {

      devmem = open("/dev/mem", O_RDWR|O_SYNC);
      if (devmem < 0) {
         fprintf(stderr, "Error:  Can't open /dev/mem\n");
         return 1;
      }
      syscon = (unsigned int *) mmap(0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem, 0xFC081000);
      if (red_led_on)
         syscon[0x0c / 4] |= (1 << 20);
      else
         syscon[0x0c / 4] &= ~(1 << 20);

      munmap((void*)syscon, 4096);

      close(devmem);
   }

   if (green_led_on && green_led_off) {
      fprintf(stderr, "green LED on and off are mutually exclusive\n");
   } else if (green_led_on || green_led_off) {

      devmem = open("/dev/mem", O_RDWR|O_SYNC);
      if (devmem < 0) {
         fprintf(stderr, "Error:  Can't open /dev/mem\n");
         return 1;
      }
      syscon = (unsigned int *) mmap(0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem, 0xFC081000);

      if (green_led_on)
         syscon[0x08 / 4] |= (1 << 30);
      else
         syscon[0x08 / 4] &= ~(1 << 30);
      munmap((void*)syscon, 4096);
      close(devmem);
   }

   if (do_info) {
      unsigned char silabs_rev;
      unsigned int clk_straps, cpu_temp;
      unsigned int pclk, nbclk, hclk, dclk, refclk, i;
      unsigned char buf[14];
      unsigned char mac[6];

      printf("model=%x\n", model);

      devmem = open("/dev/mem", O_RDWR|O_SYNC);
      if (devmem < 0) {
         fprintf(stderr, "Error:  Can't open /dev/mem\n");
         return 1;
      }
      syscon = (unsigned int *) mmap(0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem, 0xFC081000);

      if(silabs_read(twifd, &silabs_rev, 2048, 1) != 1) {
         perror("Failed to talk to silabs!");
         return 1;
      }

      printf("fpga_rev=0x%02X\n"
             "silabs_rev=%d\n"
             "board_id=0x%04X\n",
             *syscon & 0xFF, silabs_rev, (*syscon >> 8) & 0xFFFF);

      munmap((void*)syscon, 4096);
      cpuregs =   (unsigned int *) mmap(0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem, 0xf1018000);
      clk_straps = (cpuregs[0x600 / 4] >> 10) & 0x1F;
      switch(clk_straps) {
         case 0: pclk=666;nbclk=333; hclk=167; dclk=333; refclk=25;break;
         case 2: pclk=800;nbclk=400; hclk=200; dclk=400; refclk=25;break;
         case 4: pclk=1066;nbclk=533; hclk=267; dclk=533; refclk=25;break;
         case 6: pclk=1200;nbclk=600; hclk=300; dclk=600; refclk=25;break;
         case 8: pclk=1333;nbclk=667; hclk=333; dclk=667; refclk=25;break;
         case 0xc: pclk=1600;nbclk=800; hclk=400; dclk=800; refclk=25;break;
         case 0x10: pclk=1866;nbclk=933; hclk=467; dclk=933; refclk=25;break;
         default: pclk=0;nbclk=0; hclk=0; dclk=0; refclk=0;break;
      }

      munmap((void*)syscon, 4096);
      cpuregs =   (unsigned int *) mmap(0, 4096,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem, 0xf10e4000);

      cpu_temp = ((4761 * (cpuregs[0x78 / 4] & 0x3ff))) / 10 - 279100;

      printf("pclk=%d\n"
         "nbclk=%d\n"
         "hclk=%d\n"
         "dclk=%d\n"
         "refclk=%d\n"
         "cpu_temp=%d\n",
         pclk, nbclk, hclk, dclk, refclk, cpu_temp);

      memset(buf, 0, sizeof(buf));
      if(silabs_read(twifd, buf, 1280, sizeof(buf)) != sizeof(buf)) {
         perror("Failed to talk to silabs!");
         return 1;
      }

      for(i=0; i < 7; i++){
            unsigned short p = 0x3FF & *(unsigned short *)&buf[i*2];

            switch(i) {
               case 0: printf("cpu_core=%d\n", 2500 * p / 1024); break;
               case 1: printf("ram_1350=%d\n", 2500 * p / 1024); break;
               case 2: printf("v_1200=%d\n", 2500 * p / 1024); break;
               case 3: printf("v_1800=%d\n", 2500 * p / 1024); break;
               case 4: printf("v_8_30=%d\n", 2500 * p / 1024); break;
               case 5: printf("v_5va=%d\n", 5682 * p / 1024); break;
               case 6: printf("an_3300=%d\n", 5000 * p / 1024); break;
            }
         }


      silabs_read(twifd, mac, 1536, sizeof(mac));

      printf("hwaddr=%02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);

   }

   if(start_adc) {
      unsigned char buf[24];

      unsigned int i, loop=0;
      int chans;

      chans = parsechans(str, len);
         if(chans < 0) return -1;

      did_something = 1;

      while(! done) {
         short reg = 1280 + 14; // ADC regs are 1280-1535, 2 bytes each, LE

         memset(buf, 0, sizeof(buf));

         if(silabs_read(twifd, buf, reg, 10) != 10) {
            perror("Failed to write to the silabs!");
         }

         for(i=0; i < 5; i++){
            unsigned short p;
            if (! (chans & (1 << i))) continue;

            p = 0x3FF & *(unsigned short *)&buf[(4-i)*2];

            if (raw)
               printf("[0x%08X, %d]=%d\n", loop++, i, p);
            else
               printf("[0x%08X, %d]=%d\n", loop++, i, 5000 * p / 1023);

         }


         /*
         for(i=0; i < 12; i++){
            unsigned short p = 0x3FF & *(unsigned short *)&buf[i*2];

            switch(i) {
               case 0: printf("cpu_core=%d\n", 2500 * p / 1024); break;
               case 1: printf("ram_1350=%d\n", 2500 * p / 1024); break;
               case 2: printf("v_1200=%d\n", 2500 * p / 1024); break;
               case 3: printf("v_1800=%d\n", 2500 * p / 1024); break;
               case 4: printf("v_8_30=%d\n", 2500 * p / 1024); break;
               case 5: printf("v_5va=%d\n", 5682 * p / 1024); break;
               case 6: printf("an_3300=%d\n", 5000 * p / 1024); break;
               case 7: printf("adc_4=%d\n", 5000 * p / 1024); break;
               case 8: printf("adc_3=%d\n", 5000 * p / 1024); break;
               case 9: printf("adc_2=%d\n", 5000 * p / 1024); break;
               case 10: printf("adc_1=%d\n", 5000 * p / 1024); break;
               case 11: printf("adc_0=%d\n", 5000 * p / 1024); break;
            }
         }

         */

         usleep(100000);
      }
   }


   if(val_data && val_addr) {
      silabs_write(twifd, &nvram_data, 1536 + nvram_addr, sizeof(nvram_data));
   }

   if(display_odom) {
      printf("TBD: implement odometer function\n");
   }

   if(display_bday) {
        printf("TBD: implement birthdate function\n");
   }

   if (set_mac) {
      silabs_write(twifd, new_mac, 1536, sizeof(new_mac));
   }

   if(display_mac) {
      unsigned char mac[6];
      memset(mac, 0, sizeof(mac));

      if(silabs_read(twifd, mac, 1536, sizeof(mac))){
         perror("Failed to talk to silabs!");
         return 1;
      }

      printf("HWaddr %02x:%02x:%02x:%02x:%02x:%02x\n",
         mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);
   }

   if(display_mem || display_otp) {
      unsigned char buf[16];
      int i;

      memset(buf, 0, sizeof(buf));

      if(silabs_read(twifd, buf, addr, sizeof(buf))){
         perror("Failed to talk to silabs!");
         return 1;
      }

      for(i=0; i < 4; i++)
         fwrite(&buf[i * sizeof(unsigned int)], sizeof(unsigned int), 1, stdout);
      fflush(stdout);

   }

   if (did_something && secs > 0) {
      do_silabs_sleep(secs * 100);
   }

   return 0;
}



static int parsechans(const char *str, unsigned int len)
{

   int chans=0, last_ch=-1, dash=0, i, j;
   //Determine which channels to sample
   for(i=0;i<len;i++) {
      if(str[i] >= '0' && str[i] <= '4') {
         if(dash) {
            dash = 0;
            if(last_ch < 0) {
               printf("Invalid format, Sample ADC channels CHANS, e.g. \"0-2,4\", \"1,3,4\"\n");
               printf("\tCh | Pin\n");
               printf("\t---+----\n");
               printf("\t 0 | 1\n");
               printf("\t 1 | 3\n");
               printf("\t 2 | 5\n");
               printf("\t 3 | 7\n");
               printf("\t 4 | 9\n");

               return -1;
            }

            for(j=last_ch; j<=(str[i]-'0'); j++)
               chans |= (1<<j);

         } else {
            last_ch = str[i] - '0';
            chans |= 1<<(str[i] - '0');
         }

      } else if(str[i] == '-') { dash=1;
      } else if((str[i] == ',') || (str[i] == ' ')){ ;
      } else {
         printf("Invalid format, Sample ADC channels CHANS, e.g. \"0-2,4\", \"1,3,4\"\n");
         printf("\tCh | Pin\n");
         printf("\t---+----\n");
         printf("\t 0 | 1\n");
         printf("\t 1 | 3\n");
         printf("\t 2 | 5\n");
         printf("\t 3 | 7\n");
         printf("\t 4 | 9\n");
         return -1;
      }
   }

   //channel 6 => bit 4
   if(chans & (1<<6)) {
      chans |= 1<<4;
      chans &= ~(1<<6);
   }

   //channel 7 => bit 5
   if(chans & (1<<7)) {
      chans |= 1<<5;
      chans &= ~(1<<7);
   }

   return chans;
}


static int model = 0;

static int get_model(void)
{
   FILE *proc;
   char mdl[256];

   proc = fopen("/proc/device-tree/model", "r");
   if (!proc) {
       perror("model");
       return 0;
   }
   fread(mdl, 256, 1, proc);
   if (strcasestr(mdl, "TS-7800v2") || strcasestr(mdl, "TS-7800-v2"))
      return 0x7800;

   else {
      perror("model");
      return 0;
   }
}

static int silabs_init(void)
{
   static int fd = -1;
   fd = open("/dev/i2c-0", O_RDWR);
   if(fd != -1) {
      if (ioctl(fd, I2C_SLAVE_FORCE, SILABS_CHIP_ADDRESS) < 0) {
         perror("SiLabs did not ACK\n");
         return -1;
      }
   }

   return fd;
}

static int silabs_read(int twifd, uint8_t *data, uint16_t addr, int bytes)
{
   struct i2c_rdwr_ioctl_data packets;
   struct i2c_msg msgs[2];
   char busaddr[2];

   busaddr[0] = ((addr >> 8) & 0xff);
   busaddr[1] = (addr & 0xff);

   msgs[0].addr = SILABS_CHIP_ADDRESS;
   msgs[0].flags = 0;
   msgs[0].len = 2;
   msgs[0].buf = busaddr;

   msgs[1].addr = SILABS_CHIP_ADDRESS;
   msgs[1].flags = I2C_M_RD;
   msgs[1].len = bytes;
   msgs[1].buf =  (void *)data;

   packets.msgs  = msgs;
   packets.nmsgs = 2;

   if(ioctl(twifd, I2C_RDWR, &packets) < 0) {
      perror("Unable to send data");
      return 1;
   }
   return 0;
}

static int silabs_write(int twifd, uint8_t *data, uint16_t addr, int bytes)
{
   struct i2c_rdwr_ioctl_data packets;
   struct i2c_msg msg;
   uint8_t outdata[4096];

   /* Linux only supports 4k transactions at a time, and we need
    * two bytes for the address */
   assert(bytes <= 4094);

   outdata[0] = ((addr >> 8) & 0xff);
   outdata[1] = (addr & 0xff);
   memcpy(&outdata[2], data, bytes);

   msg.addr = SILABS_CHIP_ADDRESS;
   msg.flags = 0;
   msg.len  = 2 + bytes;
   msg.buf  = (char *)outdata;

   packets.msgs  = &msg;
   packets.nmsgs = 1;

   if(ioctl(twifd, I2C_RDWR, &packets) < 0) {
      return 1;
   }
   return 0;
}


/** must be in the form XX:XX:XX:XX:XX:XX, where XX is a hex number */

static int parseMacAddress(const char *str, unsigned char *buf)
{
   int n;
   unsigned int addr[6];

   if (sscanf(str, "%x:%x:%x:%x:%x:%x",
      &addr[5],&addr[4],&addr[3],
      &addr[2],&addr[1],&addr[0]) != 6)
      return 0;

   for(n=0; n < 6; n++) {
      if (addr[n] > 255) return 0;
      buf[n] = addr[n];
   }

   return 1;

}

static inline void feed_wdt(int t)
{
   int ret;
   unsigned char data[] = {
      0x20, 0x03, 0, 0  // 800
   };

   ret = silabs_write(twifd, data, 1024, sizeof(data)) != sizeof(data);
   if(!ret) {
      perror("Failed to write to the silabs!");
   }
   enable_wdt();
}

static inline void enable_wdt(void)
{
   unsigned char data = 0x1;

   if(!silabs_write(twifd, &data, 1028, 1) != 1) {
      perror("Failed to write to the silabs!");
   }
}

static inline void disable_wdt(void)
{
   unsigned char data[] = {
      0x04, 0x04, // 1028, byte-swapped
      0
   };

   if(!silabs_write(twifd, data, 1028, 1) != 1) {
      perror("Failed to write to the silabs!");
   }
}

static inline void do_silabs_sleep(unsigned int deciseconds)
{
   unsigned char data[6];

   printf("Sleeping for %d deciseconds...\n", deciseconds);

   disable_wdt();

   data[0] = deciseconds & 0xFF;
   data[1] = (deciseconds >> 8) & 0xFF;
   data[2] = (deciseconds >> 16) & 0xFF;
   data[3] = (deciseconds >> 24) & 0xFF;

   if(!silabs_write(twifd, data, 1024, 4) != 4) {
      perror("Failed to write to the silabs!");
   }

   data[0] = 2;

   if(!silabs_write(twifd, data, 1028, 1) != 1) {
      perror("Failed to write to the silabs!");
   }
}
