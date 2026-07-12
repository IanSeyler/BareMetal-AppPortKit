// helloc.c -- Output a 'hello world' message via musl's printf,
// proving the musl -> posix_shim -> libBareMetal path works end to end.
// build with build-app.sh

#include <stdio.h>

int main(int argc, char **argv, char **envp)
{
	printf("Hello, World!\n");

	printf("argc: %d\n", argc);

	for (int i = 0; i < argc; i++)
		printf("argv[%d]: %s\n", i, argv[i]);

	for (int i = 0; envp[i] != NULL; i++)
		printf("envp[%d]: %s\n", i, envp[i]);

	return 0;
}
