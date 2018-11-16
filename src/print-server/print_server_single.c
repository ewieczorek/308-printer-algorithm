/**
 * @file      main.c
 * @author    Jeramie Vens
 * @date      2015-02-11: Created
 * @date      2015-02-15: Last updated
 * @date      2016-02-16: Complete re-write
 * @date      2016-02-20: convert to single threaded
 * @brief     Emulate a print server system
 * @copyright MIT License (c) 2015, 2016
 */
 
/*
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
*/

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


#include "print_job.h"
#include "printer_driver.h"
#include "debug.h"

// -- GLOBAL VARIABLES -- //
int verbose_flag = 0;
int exit_flag = 0;
char *socket_path = "\0hidden";
char buffer[2048];
char listBuf[200];
// -- STATIC VARIABLES -- //
static struct printer_group * printer_group_head;

// -- FUNCTION PROTOTYPES -- //
static void parse_command_line(int argc, char * argv[]);
static void parse_rc_file(FILE* fp);
static void accept_socket();
static void list_printer_drivers();
/**
 * A list of print jobs that must be kept thread safe
 */
struct print_job_list
{
	// the head of the list
	struct print_job * head;
	// the number of jobs in the list
	sem_t num_jobs;
	// a lock for the list
	pthread_mutex_t lock;
};

/**
 * A printer object with associated thread
 */
struct printer
{
	// the next printer in the group
	struct printer * next;
	// the driver for this printer
	struct printer_driver driver;
	// the list of jobs this printer can pull from
	struct print_job_list * job_queue;
	// the thread id for this printer thread
	pthread_t tid;
};

/**
 * A printer group.  A group represents a collection of printers that can pull
 * from the same job queue.
 */
struct printer_group
{
	// the next group in the system
	struct printer_group * next_group;
	// the name of this group
	char * name;
	// the list of printers in this group
	struct printer * printer_queue;
	// the list of jobs for this group
	struct print_job_list job_queue;
};

int main(int argc, char* argv[])
{
	if(argc > 1){
		if(strcmp("-d", argv[1]) == 0){
			daemon(1, 0);
		}
	}

	int produce = 1;
	int n_jobs = 0;
	struct printer_group * g;
	struct printer * p;
	struct print_job * job;
	struct print_job * prev = NULL;
	char * line = NULL;
	long long job_number = 0;

	// parse the command line arguments
	//parse_command_line(argc, argv);

	// open the runtime config file
	FILE* config = fopen("config.rc", "r");
	// parse the config file
	parse_rc_file(config);
	// close the config file
	fclose(config);


	// order of opperation:
	// 1. while the exit flag has not been set
	// 2. read from standard in
	// 3. do the necessary checks from standard in
	// 4. if PRINT is received, go through each printer group, and check
	//    to see if the group name matches what is specified by the job
	// 5. Once a match is made, it puts the job in the job queue of the
	//    correct printer group
	// 6. then loop through the list of printer groups to get the print
	//    job and send it to the printers
	for(g = printer_group_head; g; g = g->next_group)
	{
		sem_init(&g->job_queue.num_jobs, 0, 0);
	}
	char configBuf[1024] = ""; 
	while(!exit_flag)
	{
		if(produce){
			accept_socket();
			char * temp;
			temp = buffer;
			//printf("\n\n\nwhat's in the buffer:\n\n%s\n\n", temp);
			
			while( (line = strsep(&temp,"\n")) != NULL ){

				if(strncmp(line, "NEW", 3) == 0)
				{
					job = calloc(1, sizeof(struct print_job));
					job->job_number = job_number++;
					strcat(configBuf,"NEW JOB MADE\n");
				}
				else if(job && strncmp(line, "FILE", 4) == 0)
				{
					strsep(&line, " ");
					size_t size = strlen(line);
					job->file_name = malloc((size_t) size);
					strncpy(job->file_name, line, size+1);
					strcat(configBuf,"FILE ADDED TO JOB: ");
					strcat(configBuf, job->file_name);
					strcat(configBuf, "\n");
				}
				else if(job && strncmp(line, "NAME", 4) == 0)
				{
					strsep(&line, " ");
					size_t size = strlen(line);
					job->job_name = malloc((size_t) size);
					strncpy(job->job_name, line, size+1);
					strcat(configBuf,"NAME ADDED TO JOB: ");
					strcat(configBuf, job->job_name);
					strcat(configBuf, "\n");
				}
				else if(job && strncmp(line, "DESCRIPTION", 11) == 0)
				{
					strsep(&line, " ");
					size_t size = strlen(line);
					job->description = malloc((size_t) size);	
					strncpy(job->description, line, size+1);
					strcat(configBuf,"DESCRIPTION ADDED TO JOB: ");
					strcat(configBuf, job->description);
					strcat(configBuf, "\n");
				}
				else if(job && strncmp(line, "PRINTER", 7) == 0)
				{
					strsep(&line, " ");
					size_t size = strlen(line);
					job->group_name = malloc((size_t) size);	
					strncpy(job->group_name, line, size+1);
					strcat(configBuf,"PRINTER ADDED TO JOB: ");
					strcat(configBuf, job->group_name);
					strcat(configBuf, "\n");
				}
				else if(job && strncmp(line, "PRINT", 5) == 0)
				{
					if(!job->group_name)
					{
						eprintf("Trying to print without setting printer\n");
						continue;
					}
					if(!job->file_name)
					{
						eprintf("Trying to print without providing input file\n");	
						continue;
					}
					for(g = printer_group_head; g; g=g->next_group)
					{
						if(strcmp(job->group_name, g->name) == 0)
						{
							printf("Printing job in %s\n", job->group_name);
 							job->next_job = g->job_queue.head;
 							g->job_queue.head = job;
 							sem_post(&g->job_queue.num_jobs);
							
							job = NULL;
							produce = 0;
							break;
						}
					}
					if(job)
					{
						eprintf("Invalid printer group name given: %s\n", job->group_name);
					}
				}
				else if(strncmp(line, "EXIT", 4) == 0)
				{
					exit_flag = 1;
				}
			}
		}else{
			for(g = printer_group_head; g; g = g->next_group){
				for(p = g->printer_queue; p; p = p->next){
					if(sem_getvalue(&p->job_queue->num_jobs, &n_jobs)){
						perror("sem_getvalue");
						abort();
					}

					if(n_jobs){
						// wait for an item to be in the list
	 					sem_wait(&p->job_queue->num_jobs);
	 					// walk the list to the end
	 					for(job = p->job_queue->head; job->next_job; prev = job, job = job->next_job);
	 					if(prev)		
	 						// fix the tail of the list
 							prev->next_job = NULL;
 						else
							// There is only one item in the list
							p->job_queue->head = NULL;
 			
	 					printf("consumed job %s\n", job->job_name);
 		
 						// send the job to the printer
						printer_print(&p->driver, job);
						produce = 1;
					}
				}
			}
		}
			strcat(configBuf, "\n\n");
			FILE *f = fopen("config.txt", "w");
			if (f == NULL)
			{
				printf("Error opening file!\n");
				exit(1);
			}

			/* print some text */
			fprintf(f, "Print job:\n%s\n", configBuf);
			fclose(f);
			fflush(stdout);
	}

	return 0;
}

/**
 * Parse the command line arguments and set the appropriate flags and variables
 * 
 * Recognized arguments:
 *   - `-v`: Turn on Verbose mode
 *   - `-?`: Print help information
 */
static void parse_command_line(int argc, char * argv[])
{
	int c;
	while((c = getopt(argc, argv, "v?")) != -1)
	{
		switch(c)
		{
			case 'v': // turn on verbose mode
				verbose_flag = 1;
				break;
			case '?': // print help information
				fprintf(stdout, "Usage: %s [options]\n", argv[0]);
				exit(0);
				break;
		}
	}
}

static void accept_socket(){
	struct sockaddr_un addr;
	char buf[2048];
	int conSock,dataSock,rc;

	socket_path="../socket";

	if ( (conSock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket error");
		exit(-1);
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (*socket_path == '\0') {
		*addr.sun_path = '\0';
		strncpy(addr.sun_path+1, socket_path+1, sizeof(addr.sun_path)-2);
	} else {
		strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path)-1);
		unlink(socket_path);
	}

	if (bind(conSock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		perror("bind error");
		exit(-1);
	}

	if (listen(conSock, 5) == -1) {
		perror("listen error");
		exit(-1);
	}
	int exiter = 0;
	while (!exiter) {
		if ( (dataSock = accept(conSock, 0, 0)) == -1) {
			perror("accept error");
			continue;
		}
		while ( (rc=read(dataSock,buf,sizeof(buf))) > 0) {
			exiter = 1;
			break;
		}
		if (rc == -1) {
			perror("read");
			exit(-1);
		}
		else if (rc == 0) {
			printf("EOF\n");
		}
	}
	list_printer_drivers();
	if(!strncmp(buf, "LIST_DRIVERS", 12)){
		if(write(dataSock, listBuf, sizeof(listBuf)) != sizeof(listBuf)){
			perror("write error");
		}
		close(conSock);
		accept_socket();
	}else{
		strcpy(buffer, buf);
	}
	close(conSock);
}

static void list_printer_drivers(){
	memset(listBuf, 0, 200);
	struct printer_group * g;
	struct printer * p;
	for(g = printer_group_head; g; g=g->next_group){
		for(p = g->printer_queue; p; p = p->next){
			strcat(listBuf, p->driver.name);
			strcat(listBuf, "|");	
			strcat(listBuf, g->name);
			strcat(listBuf, "\n");
		}
	}
}

static void parse_rc_file(FILE* fp)
{
	char * line = NULL;
	char * ptr;
	size_t n = 0;
	struct printer_group * group = NULL;
	struct printer_group * g;
	struct printer * printer = NULL;
	struct printer * p;

	// get each line of text from the config file
	while(getline(&line, &n, fp) > 0)
	{
		// if the line is a comment
		if(line[0] == '#')
				continue;

		// If the line is defining a new printer group
		if(strncmp(line, "PRINTER_GROUP", 13) == 0)
		{
			strtok(line, " ");
			ptr = strtok(NULL, "\n");
			group = calloc(1, sizeof(struct printer_group));
			group->name = malloc(strlen(ptr)+1);
			strcpy(group->name, ptr);

			if(printer_group_head)
			{
				for(g = printer_group_head; g->next_group; g=g->next_group);
				g->next_group = group;
			}
			else
			{
				printer_group_head = group;
			}
		}
		// If the line is defining a new printer
		else if(strncmp(line, "PRINTER", 7) == 0)
		{
			strtok(line, " ");
			ptr = strtok(NULL, "\n");
			printer = calloc(1, sizeof(struct printer));
			printer_install(&printer->driver, ptr);
			printer->job_queue =  &(group->job_queue);
			if(group->printer_queue)
			{
				for(p = group->printer_queue; p->next; p = p->next);
				p->next = printer;
			}
			else
			{
					group->printer_queue = printer;

			}
		}
	}

	// print out the printer groups
	dprintf("\n--- Printers ---\n"); 
	for(g = printer_group_head; g; g = g->next_group)
	{
		dprintf("Printer Group %s\n", g->name);
		for(p = g->printer_queue; p; p = p->next)
		{
			dprintf("\tPrinter %s\n", p->driver.name);
		}
	}
	dprintf("----------------\n\n");

}

