#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/poll.h>

#include "tee-benchmarking.h"

#define meg     1000000


static int align_mask = 65535;
static int splice_flags;
clock_t start, end;


static int parse_options(int argc, char *argv[])
{
	int opt, index = 1;

	while ((opt = getopt(argc, argv, ":gu")) != -1) {
		switch (opt) {
			case 'u':
				splice_flags = SPLICE_F_MOVE;
				index++;
				break;
			case 'g':
				printf("gift\n");
				splice_flags = SPLICE_F_GIFT;
				index++;
				break;
			default:
				return -1;
		}
	}
    for(; optind <= argc; optind++){      
        printf("extra arguments: %s\n", argv[optind-1]);  
    } 

	return index;
}

int do_vmsplice(int fd, char **data)
{
    int page_counter = K_MULTIPLY - 1;
	struct iovec iov[] = {
		{
			.iov_base = data[page_counter],
			.iov_len = SPLICE_SIZE,
		},
	};
	int written, idx = 0, nread = 0;
	while (page_counter >= 0) {
		/*
		 * in a real app you'd be more clever with poll of course,
		 * here we are basically just blocking on output room and
		 * not using the free time for anything interesting.
		 */
		written = svmsplice(fd, &iov[idx], 1, splice_flags);
        printf("here");

		if (written <= 0)
			return error("vmsplice");

		if ((size_t) written >= iov[idx].iov_len) {
			int extra = written - iov[idx].iov_len;
            nread+=written;
			page_counter--;
			iov[idx].iov_len = SPLICE_SIZE;
			iov[idx].iov_base = data[page_counter];
		} else {
            nread+=written;
			iov[idx].iov_len -= written;
			iov[idx].iov_base += written;
		}
	}

    if(nread < 0){
        return -1;
    }
	return nread;
}

int do_tee(int fdin, int fdout){
    int len = K_MULTIPLY * SPLICE_SIZE;
    int written = 0, nread = 0;
	while (len > 0) {
		written = stee(fdin, fdout, INT_MAX, SPLICE_F_MOVE);
        if (written < 0)
            return error("tee\n");
        else{len -= written;}
	}
	printf("len = %d .\n", len);
    return K_MULTIPLY*SPLICE_SIZE;
}

int do_splice(int fdin, int fdout){
    struct pollfd pfdout = { .fd = fdout, .events = POLLOUT, };
    int len = K_MULTIPLY * SPLICE_SIZE;
    int written = 0, nread = 0;
	
	while (len > 0) {
		// if (poll(&pfdout, 1, -1) < 0)
			// return error("poll");
		written = ssplice(fdin, NULL, STDOUT_FILENO, NULL, len, SPLICE_F_MOVE);
        if (written < 0)
            return error("splice");
        len -= written;
	}
	// printf("len = %d .\n", len);
    return K_MULTIPLY*SPLICE_SIZE;
}

int main(int argc, char *argv[]) 
{   
    if (parse_options(argc, argv) < 0)
		return usage(argv[0]);
    
	ssize_t nread;
	char* name;
	int pip[2], fd[2], child_pipe[2];
	if (pipe(child_pipe) < 0 || pipe(pip) < 0 || pipe(fd) < 0) 
		exit(1);

	char** data1 = empty_allocator();
	char** data = initializer();


	pid_t   childpid;
	if((childpid = fork()) == -1)
	{
		perror("fork");
		exit(1);
	}
	if(childpid == 0)
	{
		/* First Child process closes up input side of pipe. */
		name = "First Child";
		close(pip[0]);
		close(fd[0]);

		start = clocker(0, name);   
		// size_printer(name);
		nread = do_vmsplice(pip[1], data);
		close(pip[0]);
		end = clocker(1, name);

		double result = time_calc(end, start, name);;
		printf("---------------------------------------------\n");
		write(fd[1], &start, sizeof(start));
		write(fd[1], &end, sizeof(start));	    
		exit(0);
	}
	else
	{
        pid_t child2_pid;
        if((child2_pid = fork()) == -1)
        {
            error("fork");
            exit(1);
        }
        if(child2_pid == 0)
	    {
			pid_t child3_pid;
			if((child3_pid = fork()) == -1)
			{
				error("fork");
				exit(1);
			}
			if(child3_pid == 0)
	    	{
				close(fd[1]);
				close(pip[1]);             
				close(child_pipe[1]);
				pid_t child4_pid;
				if((child4_pid = fork()) == -1)
				{
					error("fork");
					exit(1);
				}
				if(child4_pid == 0)
	    		{
					name = "Second Splicer Child 2";
					nread = do_splice(child_pipe[0], STDOUT_FILENO);
					exit(0);
				}
				else{
					name = "Second Splicer Child 1";
					nread = do_splice(pip[0], STDOUT_FILENO);
					int child4_status;
            		waitpid(child4_pid, &child4_status, 0);
					printf("in %s: number of reads from the pipe = %ld\n", name, nread);
					
					exit(0);
				}
			}
			else{
				/* Second Child process closes up output side of pipe */
				name = "Second Tee Child";

				close(fd[1]);
				close(pip[1]);             
				close(child_pipe[0]);

				start = clocker(0, name);
				// size_printer(name);
				nread = do_tee(pip[0], child_pipe[1]);
				end = clocker(1, name);
				
				int child3_status;
            	waitpid(child3_pid, &child3_status, 0);
				if (child3_status == 0)  // Verify child process terminated without error.  
				{
					printf("The SPLICER processes terminated normally.\n");    
				}
				else{printf("The SPLICER process terminated with AN ERROR!.\n");}

				printf("in %s: number of reads from the pipe = %ld\n", name, nread);
				printf("---------------------------------------------\n");
				clock_t first_start;
				clock_t first_end;
				read(fd[0], &first_start, sizeof(start));
				read(fd[0], &first_end, sizeof(start));
				double first_result = time_calc(first_end, first_start, "First Child");
				printf("---------------------------------------------\n");
				double result = time_calc(end, start, name);
				printf("---------------------------------------------\n");
				result = time_calc(end, first_start, "realtime_calculation");
		        printf("The frequency is eqals to(Mbps): %f .\n", (double)((2* K_MULTIPLY*SPLICE_SIZE)/(result*meg)));

				// printf("time to write and read into the pipe by vmsplice = %f\n", result);
				exit(0);
			}
        }
        else{
            /* Parent process closes up output side of pipe */
            int child1_status, child2_status;
            waitpid(childpid, &child1_status, 0);
            waitpid(child2_pid, &child2_status, 0);
            if (child1_status == 0 && child2_status == 0)  // Verify child process terminated without error.  
            {
                printf("The child processes terminated normally.\n");    
            }
            else{printf("The child process terminated with an error!.\n");}
			free_allocator(data);
            free_allocator(data1);

            exit(0);
        }
	}
    return 0;
}
