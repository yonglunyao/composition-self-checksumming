#include <stdint.h>
#include <stdio.h>
#include <execinfo.h>
#include <stdlib.h>
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

void guardMe(const unsigned int address, const unsigned int length, const unsigned int expectedHash) {

  const unsigned char *beginAddress = (const unsigned char *) address;
  unsigned int visited = 0;
  unsigned char hash = 0;
  //Note: Length need to be divided by the size of
  //type of begin address that we are reading each time,
  //otherwise it falls out of the scope (see #3)
//	printf("%sLength:%d Begin address:%d Expectedhash:%d\n",KRED,length,address,expectedHash);
  while (visited < length) {
//		printf("%x ",*beginAddress);
    hash ^= *beginAddress++;
    ++visited;
  }
//	printf("\n");

//	printf("%sruntime hash: %x\n",KGRN,hash);
//	printf("%sexpected hash: %x\n",KBLU,expectedHash);
//	printf("%s",KNRM);

  if (hash != (unsigned char) expectedHash) {
    //response();
    printf("%sTampered binary!\n", KNRM);

    void *callstack[128];
    int i, frames = backtrace(callstack, 128);
    char **strs = backtrace_symbols(callstack, frames);

    for (i = 0; i < frames; ++i) {
      printf("%s\n", strs[i]);
    }

    free(strs);
    exit(777);
  }
}

//void respone(){
//	printf("Tampered binary!");
//}
//void dbghashMe(int i, std::string valueName){
//	printf("adding hash %s %i\n",valueName, i);
//        hash +=i;
//}
void logHash() {
  //printf("final hash: %ld\n", hash);
}
