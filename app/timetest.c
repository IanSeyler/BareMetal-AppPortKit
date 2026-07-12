#include <stdio.h>
#include <time.h>

int main(void)
{
	time_t t = time(NULL);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	printf("time()=%ld clock_gettime sec=%ld nsec=%ld\n", (long)t, (long)ts.tv_sec, (long)ts.tv_nsec);

	struct tm tm;
	gmtime_r(&t, &tm);
	printf("%04d-%02d-%02d %02d:%02d:%02d UTC (weekday=%d yday=%d)\n",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		tm.tm_wday, tm.tm_yday);

	char buf[64];
	strftime(buf, sizeof(buf), "%A, %B %d %Y %H:%M:%S UTC", &tm);
	printf("%s\n", buf);

	struct timespec bad;
	int r = clock_gettime(CLOCK_MONOTONIC, &bad);
	printf("CLOCK_MONOTONIC result=%d\n", r);
	return 0;
}
