
//TEST ANONYMOUS MAPPING
#include "types.h"
#include "user.h"
int main() {
    // 파일을 메모리에 매핑합니다. 여기서는 예제로 4096 바이트를 매핑합니다.
    int length = 4096;
    int protection = 0x1 | 0x2;
    int flags = 0x1;

    printf(1,"freemem0: %d \n", freemem());

    uint addr = mmap(0, length, protection, flags, -1, 0);
    
    if (addr == 0) {
        printf(1,"mmap failed");
        return 0;
    }

    printf(1,"Memory mapped at address %p\n", addr);

    // 매핑된 메모리에 데이터를 작성합니다.
    char* data = (char*) addr;
    for (int i = 0; i < length; ++i) {
        data[i] = 'a';
    }

    // 데이터를 확인합니다.
    printf(1,"1st byte: %c\n", data[0]);
    printf(1,"2nd byte: %c\n", data[1]);
    printf(1,"3rd byte: %c\n", data[2]);
    
    printf(1,"freemem1: %d \n", freemem());
    // 메모리 매핑을 해제합니다.
    if (munmap(addr) == -1) {
        printf(1,"munmap failed\n");
        return 0;
    }
    printf(1,"freemem2: %d \n", freemem());
    printf(1, "Memory unmapped\n");

    //return 0;
    exit();
}
/*

//FILE MAPPING WITH MAP_POPULATE 
#include "types.h"
#include "user.h"
#include "fcntl.h"
int main() {
    int length = 4096;
    int protection = 0x1; //읽기/쓰기 권한 설정
    //int flags = 0x2; // map populate 설정
    int fd = open("README", O_RDONLY); // 예제 파일 열기
    printf(1,"fd: %d\n", fd);
    if (fd < 0) {
        printf(1, "Failed to open file\n");
        exit();
    }

    // 파일 매핑을 설정합니다. fd는 열린 파일의 디스크립터입니다.
    uint addr = mmap(0, length, protection, 0x2, fd, 0);
    if (addr == 0) {
        printf(1,"mmap failed with address 0\n");
        close(fd);
        exit();
    }

    printf(1,"Memory mapped at address %p\n", addr);

    // 매핑된 메모리에서 데이터를 읽어옵니다.
    char* data = (char*) addr;
    printf(1,"1st byte: %c\n", data[0]); // 첫 번째 바이트 출력
    printf(1,"2nd byte: %c\n", data[1]); // 두 번째 바이트 출력
    printf(1,"3rd byte: %c\n", data[2]); // 세 번째 바이트 출력

    // 파일 매핑을 해제하고 파일을 닫습니다.
    if (munmap(addr) == -1) {
        printf(1,"munmap failed\n");
        close(fd);
        exit();
    }

    printf(1, "Memory unmapped\n");
    close(fd);
    return 0;
}


//file mapping without map_populate
#include "types.h"
#include "user.h"
#include "fcntl.h"
int main() {
    int length = 4096;
    int protection = 0x1; //읽기/쓰기 권한 설정
    //int flags = 0x2; // map populate 설정
    int fd = open("README", O_RDONLY); // 예제 파일 열기
    printf(1,"fd: %d\n", fd);
    if (fd < 0) {
        printf(1, "Failed to open file\n");
        exit();
    }

    // 파일 매핑을 설정합니다. fd는 열린 파일의 디스크립터입니다.
    uint addr = mmap(0, length, protection, 0, fd, 0);
    if (addr == 0) {
        printf(1,"mmap failed with address 0\n");
        close(fd);
        exit();
    }

    printf(1,"Memory mapped at address %p\n", addr);

    // 매핑된 메모리에서 데이터를 읽어옵니다.
    char* data = (char*) addr;
    printf(1,"1st byte: %c\n", data[0]); // 첫 번째 바이트 출력
    printf(1,"2nd byte: %c\n", data[1]); // 두 번째 바이트 출력
    printf(1,"3rd byte: %c\n", data[2]); // 세 번째 바이트 출력

    // 파일 매핑을 해제하고 파일을 닫습니다.
    if (munmap(addr) == -1) {
        printf(1,"munmap failed\n");
        close(fd);
        exit();
    }

    printf(1, "Memory unmapped\n");
    close(fd);
    return 0;
}
*/