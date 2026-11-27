# 핵심 계층. ESP-IDF·FreeRTOS 헤더를 쓰지 않는다(REQUIRES 없음). port_* 함수는 main 컴포넌트가 준다.
idf_component_register(SRCS "src/isotp.c" "src/diag.c" "src/policy.c" "src/busguard.c" "src/stats.c" "src/dtc.c" "src/cli.c" "src/out.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
