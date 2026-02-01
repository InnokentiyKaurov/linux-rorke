#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#define SCHED_RORKE 8

unsigned long long fib(unsigned long long n)
{
	if (n <= 1)
		return n;
	else
		return fib(n - 1) + fib(n - 2);
}

int main(int argc, char** argv)
{
	int n = atoi(argv[1]);
	struct sched_param params = {0};
	int ret = sched_setscheduler(0, SCHED_RORKE, &params);
	if (ret)
	{
		fprintf(stderr, "WFQ scheduling policy does not exist\n");
		exit(1);
	}

	if (argc != 2)
	{
		fprintf(stderr, "Usage: ./fibonacci [number]\n");
		exit(1);
	}

	printf("fib(%d) = %llu (pid %d) \n", n, fib(n), getpid());
	return 0;
}
