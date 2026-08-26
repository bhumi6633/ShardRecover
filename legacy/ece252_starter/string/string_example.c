#include <stdio.h>
#include <string.h>
#include <ctype.h> // for isalnum

int main() {
    char str1[50] = "Hello, world! 123";
    char str2[50] = "world";
    char str3[50];

    // strlen
    printf("Length of str1: %zu\n", strlen(str1));

    // strcpy
    strcpy(str3, str1);
    printf("Copied str1 to str3: %s\n", str3);

    // strcat
    strcat(str3, " Have a nice day!");
    printf("After concatenation: %s\n", str3);

    // strcmp
    int cmp = strcmp(str1, str2);
    printf("strcmp(str1, str2): %d\n", cmp);

    // strstr: Find substring
    char *p = strstr(str1, str2);
    if (p) {
        printf("Substring '%s' found in str1 at position %ld\n", str2, p - str1);
    } else {
        printf("Substring '%s' not found in str1\n", str2);
    }

    // isalnum: Analyze each character in str1
    printf("Alphanumeric characters in str1: '");
    for (int i = 0; str1[i]; ++i) {
        if (isalnum((unsigned char)str1[i])) {
            putchar(str1[i]);
        }
    }
    printf("'\n");

    // memset
    memset(str3, '*', 5);
    str3[5] = '\0';
    printf("After memset: %s\n", str3);

    // memcpy
    memcpy(str3, str1, 5);
    str3[5] = '\0';
    printf("After memcpy: %s\n", str3);

    return 0;
}
