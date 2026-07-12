// helloc.c -- Output a 'hello world' message via musl's printf,
// proving the musl -> posix_shim -> libBareMetal path works end to end.
// build with build-app.sh

#include <stdio.h>

int main(int argc, char **argv, char **envp)
{
	(void)argc; (void)argv; (void)envp;

	printf("Hello, World!\n");

	return 0;
}
