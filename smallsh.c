#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct stCommand {
	char *arrsz_args [512];
	int fdIn;
	int fdOut;
	int bForeground;
} stCommand;

typedef struct stProcessList {
	pid_t pidSelf;
	struct stProcessList *prNext;
	int fdIn;
	int fdOut;
} stProcessList;

/* Given the address to a string (**pszStr) and its actual length in bytes (u32Size), resizes that string
 * by doubling the amount of bytes allocated for it until it occupies at least u32Goal bytes in memory.
 * Returns the new (if u32Size began less than u32Goal) size of the string. */
size_t fnResizeString(char **pszStr, size_t u32Size, size_t u32Goal);

/* Given a pathname, if its first character is ~, substitutes that character with the full path to HOME
 * Allocates at least strlen(szPathname) + 1 bytes of memory, which must be freed manually. */
char *fnPnPrependHomeMaybe(char *szPathname);

/* Given an array of strings representing command line arguments, creates a new stCommand struct */
int fnStCommandCreate(stCommand *comC, char *arrszArgs [512], int bForegroundOnly);

/* Frees any memory associated with a stCommand object, assuming the stCommand object is stack-allocated itself */
void fnStCommandFree(stCommand *comC);

/* Frees a stack-allocated char array of maximum size 512 */
void fnFreeArgs(char **parrszArgs);

/* create and push a new process onto process list */
void fnStProcessListPush(stProcessList **plExisting, pid_t pidNew, int fdIn, int fdOut);

/* remove process with matching pid from process list, if it exists, and return a pointer to it */
stProcessList *fnStProcessListPopMatching(stProcessList **plProcessList, pid_t pidRm);

/* SIGTSTP signal handler */
void hSIGTSTP(int iSigno);


int bForegroundOnly = 0; /* GLOBAL VARIABLE indicating whether or not foreground-only mode is active */

int main(int argc, char **argv)
{
	/* set up SIGINT action for parent process */
	struct sigaction saSIGINT = {{ 0 }}; /* this {{}} business is apparently due to a GCC bug */
	saSIGINT.sa_handler = SIG_IGN;
	sigfillset(&saSIGINT.sa_mask);
	saSIGINT.sa_flags = 0;
	sigaction(SIGINT, &saSIGINT, NULL);

	/* set up SIGTSTP action for parent process */
	struct sigaction saSIGTSTP = {{ 0 }};
	saSIGTSTP.sa_handler = hSIGTSTP;
	sigfillset(&saSIGTSTP.sa_mask);
	saSIGTSTP.sa_flags = SA_RESTART;
	sigaction(SIGTSTP, &saSIGTSTP, NULL);

	char *szLine = NULL; /* storage for command entered by the user */
	size_t u32Lsz = 0; /* allocated size for the command */
	int bLastFgPSigTerm = 0; /* 0 -- last process exited via exit, 1 -- last process was terminated by a signal */
	int sLastFgPStat = 0; /* status of last foreground process */

	stProcessList *plBackgroundProcesses = NULL; /* linked list containing open file descriptors and process ids of all running/waiting/zombie background processes */

	/* main process loop */
	while (1) {
		printf(": ");
		fflush(stdout);

		getline(&szLine, &u32Lsz, stdin);

		/* handling of comments and blank lines */
		if (*szLine == '#' || *szLine == '\n') {
			/* wait on all background processes-- check if any have resolved */
			for (stProcessList *plI = plBackgroundProcesses; plI != NULL; ) {
				int sBackground;
				pid_t pidBc = waitpid(plI->pidSelf, &sBackground, WNOHANG);
				/* for each pid that is resolved, pop it off of the process list */
				if (pidBc != 0) {
					stProcessList *prDone = fnStProcessListPopMatching(&plBackgroundProcesses, pidBc);
					if (prDone->fdIn != STDIN_FILENO) { close(prDone->fdIn); }
					if (prDone->fdOut != STDOUT_FILENO) { close(prDone->fdOut); }
					if (prDone == plI) { plI = plI->prNext; }
					free(prDone);
					printf("DONE with background process with pid [%d]: ", pidBc);
					if (WIFEXITED(sBackground)) {
						printf("Exited with status %d\n", WEXITSTATUS(sBackground));
					} else {
						printf("Terminated by signal %d\n", WTERMSIG(sBackground));
					}
				}
				if (pidBc == 0) { plI = plI->prNext; }
			}
			continue;
		} else {
			char *arrszArgs [512] = { NULL }; /* array of command arguments */
			/* separate the command into arguments, substituting the PID expansion as appropriate */
			{
				char szPid [16]; /* storage for the process id. it is 16 bytes because this will accomodate the maximum possible pid in string form */
				memset(szPid, '\0', 16);
				int iPidLen = sprintf(szPid, "%d", getpid()); /* length of pid in string form */

				const char szDelim[] = " \n"; /* delimiter string for command-- strip all spaces and newlines */
				char *pSave = NULL; /* save pointer for strtok_r */
				int i = 0;
				char *szTok = strtok_r(szLine, szDelim, &pSave); 
				/* this is the part that separates the command into arguments */
				for ( ; szTok != NULL && i < 511; szTok = strtok_r(NULL, szDelim, &pSave), i++) {
					size_t u32Acap = strlen(szTok) + 1; /* capacity of argstring-- number of bytes it literally occupies in memory */
					size_t u32Asz = u32Acap; /* size of argstring-- number of characters it currently occupies, including nullterm */
					arrszArgs[i] = strdup(szTok);
					/* repeatedly replace any instances of $$ in the string with the process ID */
					for (char *szExp = strstr(arrszArgs[i], "$$"); szExp != NULL; szExp = strstr(arrszArgs[i], "$$")) {
						char *szAftExp = strdup(szExp + 2);
						u32Asz += iPidLen - 2;
						u32Acap = fnResizeString(arrszArgs + i, u32Acap, u32Asz);
						szExp = strstr(arrszArgs[i], "$$");
						strcpy(szExp, szPid);
						strcpy(szExp + iPidLen, szAftExp);
						free(szAftExp);
					}
				}
			}

			/* section for handling commands */
			/* it is still possible for no memory to be allocated to any position in arrszArgs, so just continue if this happens */
			if (*arrszArgs == NULL || strcmp("<", *arrszArgs) == 0 || strcmp(">", *arrszArgs) == 0 || strcmp("&", *arrszArgs) == 0) {
				goto CLEANUP;
			} else if (strcmp("exit", *arrszArgs) == 0) {
				fnFreeArgs(arrszArgs);
				while (plBackgroundProcesses != NULL) {
					stProcessList *temp = plBackgroundProcesses;
					plBackgroundProcesses = plBackgroundProcesses->prNext;
					free(temp);
				}
				goto EXIT;
			} else if (strcmp("cd", *arrszArgs) == 0) {
				char *szOdir = getcwd(NULL, 0);
				char *szHome = getenv("HOME");

				if (arrszArgs[1] == NULL || strcmp("&", arrszArgs[1]) == 0) {
					if (chdir(szHome) == 0) {
						setenv("PWD", szHome, 1);
						printf("WAS %s\n", szOdir);
					}
				} else {
					/* the chdir function doesn't seem to support ~ (i can find literally no documentation on this),
 					   so if the first character of arrszArgs[1] is ~, it sets the directory to change to to home
					   concatenated with the desired directory */
					char *szChd = fnPnPrependHomeMaybe(arrszArgs[1]);

					if (chdir(szChd) == 0) {
						char *szCwd = getcwd(NULL, 0);
						setenv("PWD", szCwd, 1);
						printf("WAS %s\n", szOdir);
						free(szCwd);
					} else {
						switch (errno) {
							case ENOTDIR:
								printf("CD: Cannot change to %s: Not a directory\n", szChd);
								break;
							default:
								printf("CD: %s: No such file or directory\n", szChd);
						}
					}

					fflush(stdout);

					free(szChd);
				}

				free(szOdir);
			} else if (strcmp("status", *arrszArgs) == 0) {
				if (bLastFgPSigTerm) {
					printf("LAST FOREGROUND PROCESS TERMINATED by signal %d\n", sLastFgPStat);
				} else {
					printf("LAST FOREGROUND PROCESS EXITED with status %d\n", sLastFgPStat);
				}
			} else {
				/* create command struct */
				stCommand comC;
				int idMyErrno = fnStCommandCreate(&comC, arrszArgs, bForegroundOnly);
				if (idMyErrno != 0) {
					switch(idMyErrno) {
						case 1:
							puts("SMALLSH: Filename expected after < token");
							break;
						case 2:
							puts("SMALLSH: Filename expected after > token");
							break;
						case 3:
							puts("SMALLSH: Unexpected token");
							break;
						case 4: puts("SMALLSH: Input file could not be opened");
							break;
						case 5:
							puts("SMALLSH: Output file could not be opened");
					}

					fflush(stdout);
				} else {
					/* fork and run the command! */
					pid_t pidC = fork();
					switch (pidC) {
						case -1:
							perror("SMALLSH: FATAL ERROR");
							exit(1);
						case 0:
							/* all child processes ignore SIGTSTP */
							saSIGTSTP.sa_handler = SIG_IGN;
							saSIGTSTP.sa_flags = 0;
							sigaction(SIGTSTP, &saSIGTSTP, NULL);

							if (comC.bForeground) {
								/* reregister signal handler */
								saSIGINT.sa_handler = SIG_DFL;
								sigaction(SIGINT, &saSIGINT, NULL);
							}

							/* set up IO redirection */
							if (dup2(comC.fdIn, STDIN_FILENO) == -1) { perror("SMALLSH: DUP2"); }
							if (dup2(comC.fdOut, STDOUT_FILENO) == -1) { perror("SMALLSH: DUP2"); }

							/* execute the process; clean up and return error code 1 on failure */
							execvp(comC.arrsz_args[0], comC.arrsz_args);
							perror("SMALLSH: EXECVP");
							fnStCommandFree(&comC);
							fnFreeArgs(arrszArgs);
							while (plBackgroundProcesses != NULL) {
								stProcessList *temp = plBackgroundProcesses;
								plBackgroundProcesses = plBackgroundProcesses->prNext;
								free(temp);
							}
							free(szLine);
							exit(1);
						default:
							/* block and wait for any foreground process to terminate, then update sLastFgPStat */
							if (comC.bForeground) {
								int sCh;
								waitpid(pidC, &sCh, 0);
								if (bLastFgPSigTerm = WIFSIGNALED(sCh)) {
									sLastFgPStat = WTERMSIG(sCh);
									printf("TERMINATED with signal %d\n", sLastFgPStat);
								} else {
									sLastFgPStat = WEXITSTATUS(sCh);
								}
								if (comC.fdIn != STDIN_FILENO) { close(comC.fdIn); }
								if (comC.fdOut != STDOUT_FILENO) { close(comC.fdOut); }
							} else {
								/* print the message and add the process to the list of background processes */
								printf("BACKGROUND pid is [%d]\n", pidC);

								fnStProcessListPush(&plBackgroundProcesses, pidC, comC.fdIn, comC.fdOut);
							}
					}
				}

				fnStCommandFree(&comC);
			}

			CLEANUP:
			/* wait on all background processes-- check if any have resolved */
			for (stProcessList *plI = plBackgroundProcesses; plI != NULL; ) {
				int sBackground;
				pid_t pidBc = waitpid(plI->pidSelf, &sBackground, WNOHANG);
				/* for each pid that is resolved, pop it off of the process list */
				if (pidBc != 0) {
					stProcessList *prDone = fnStProcessListPopMatching(&plBackgroundProcesses, pidBc);
					if (prDone->fdIn != STDIN_FILENO) { close(prDone->fdIn); }
					if (prDone->fdOut != STDOUT_FILENO) { close(prDone->fdOut); }
					if (prDone == plI) { plI = plI->prNext; }
					free(prDone);
					printf("DONE with background process with pid [%d]: ", pidBc);
					if (WIFEXITED(sBackground)) {
						printf("Exited with status %d\n", WEXITSTATUS(sBackground));
					} else {
						printf("Terminated by signal %d\n", WTERMSIG(sBackground));
					}
				}
				if (pidBc == 0) { plI = plI->prNext; }
			}

			if (*arrszArgs != NULL) { fnFreeArgs(arrszArgs); }
		}
	}

	EXIT:
	free(szLine);

	return 0;
}

size_t fnResizeString(char **pszStr, size_t u32Start, size_t u32Goal)
{
	/* resize the string while it is not big enough */
	while (u32Goal > u32Start) {
		char *szTemp = strcpy(calloc(u32Start *= 2, 1), *pszStr);
		free(*pszStr);
		*pszStr = szTemp;
	}

	return u32Start;
}

char *fnPnPrependHomeMaybe(char *szPathname) {
	char *szHome = getenv("HOME");
	/* return the argument prepended to HOME if the first character of the argument is ~ */
	return *szPathname == '~' ? strcat(strcpy(calloc(strlen(szHome) + strlen(szPathname), 1), szHome), szPathname + 1) :
				    strdup(szPathname);
}

int fnStCommandCreate(stCommand *comC, char *arrszArgs [512], int bForegroundOnly)
{
	for (char **pszArg = comC->arrsz_args; pszArg < comC->arrsz_args + 512; pszArg++) { *pszArg = NULL; }
	comC->fdIn = STDIN_FILENO;
	comC->fdOut = STDOUT_FILENO;
	comC->bForeground = 1;

	/* represents the state of a simple state machine used to parse the command array properly 
 	 * 0 = initial state
 	 * 1 = about to accept an input filename argument
 	 * 2 = expecting either > or & (done accepting arguments for the command)
 	 * 3 = about to accept an output filename argument
 	 * 4 = expecting an immediate & at the end of the argument string */
	int sPhase = 0;

	/* represents an error condition
         * 0 = no error
         * 1 = expected filename after < token 
         * 2 = expected filename after > token 
         * 3 = unexpected token
         * 4 = input file could not be opened
         * 5 = output file could not be opened */
	int idMyErrno = 0;
	int i = 0;
	for (char **pS = arrszArgs; *pS != NULL && pS < arrszArgs + 511 && idMyErrno == 0; pS++) {
		switch (sPhase) {
			case 0:
				if (strcmp("<", *pS) == 0) {
					/* both this and the one for > set my errno if they do not have a following element */
					if (*(pS + 1) != NULL) {
						sPhase = 1;
					} else {
						idMyErrno = 1;
					}
				} else if (strcmp(">", *pS) == 0) {
					if (*(pS + 1) != NULL) {
						sPhase = 3;
					} else {
						idMyErrno = 2;
					}
				/* set background mode and set up IO redirecton to /dev/null */
				} else if (strcmp("&", *pS) == 0 && *(pS + 1) == NULL) {
					if (!bForegroundOnly) {
						comC->bForeground = 0;
						comC->fdIn = open("/dev/null", O_RDONLY);
						comC->fdOut = open("/dev/null", O_WRONLY);
					}
				} else {
					/* copy over the arguments one by one */
					comC->arrsz_args[i++] = strdup(*pS);
				}
				break;
			case 1: {
				/* set up input redirection */
				char *szPathname = fnPnPrependHomeMaybe(*pS);
				if ((comC->fdIn = open(szPathname, O_RDONLY, 0640)) < 0) {
					idMyErrno = 4;
				} else {
					sPhase = 2;
				}
				free(szPathname);
				break;
			} case 2:
				if (strcmp(">", *pS) == 0) {
					if (*(pS + 1) != NULL) {
						sPhase = 3;
					} else {
						idMyErrno = 2;
					}
				} else if (strcmp("&", *pS) == 0 && *(pS + 1) == NULL) {
					/* set up output redirection and conditionally input redirection to /dev/null */
					if (!bForegroundOnly) {
						comC->bForeground = 0;
						if (comC->fdIn == STDIN_FILENO) { comC->fdIn = open("/dev/null", O_RDONLY); }
						comC->fdOut = open("/dev/null", O_WRONLY);
					}
				} else {
					idMyErrno = 3;
				}
				break;
			case 3: {
				char *szPathname = fnPnPrependHomeMaybe(*pS);
				if ((comC->fdOut = open(szPathname, O_CREAT | O_TRUNC | O_WRONLY, 0640)) < 0) {
					idMyErrno = 5;
				} else {
					sPhase = 4;
				}
				free(szPathname);
				break;
			} case 4:
				if (strcmp("&", *pS) != 0 || *(pS + 1) != NULL) {
					idMyErrno = 3;
				} else if (!bForegroundOnly) {
					/* set up input and output redirection for the background process if it has not been set up yet */
					comC->bForeground = 0;
					if (comC->fdIn == STDIN_FILENO) { comC->fdIn = open("/dev/null", O_RDONLY); }
					if (comC->fdOut == STDOUT_FILENO) { comC->fdOut = open("/dev/null", O_WRONLY); }
				}
		}
	}

	return idMyErrno;
}

void fnStCommandFree(stCommand *comC)
{
	for (char **pS = comC->arrsz_args; *pS != NULL && pS < comC->arrsz_args + 511; pS++) {
		free(*pS);
	}
}

void fnFreeArgs(char **parrszArgs)
{
	for (char **pS = parrszArgs; *pS != NULL && pS < parrszArgs + 511; pS++) {
		free(*pS);
	}
}

void fnStProcessListPush(stProcessList **plExisting, pid_t pidNew, int fdIn, int fdOut)
{
	/* find the end of the process list */
	stProcessList *plL = *plExisting;
	for (stProcessList *plI = *plExisting; plI != NULL; plI = plI->prNext) { plL = plI; }

	/* allocate new process */
	stProcessList *plNew = malloc(sizeof(stProcessList));
	plNew->pidSelf = pidNew;
	plNew->prNext = NULL;
	plNew->fdIn = fdIn;
	plNew->fdOut = fdOut;

	/* append to the end of the list, or initiate as the head of the list */
	if (plL != NULL) {
		plL->prNext = plNew;
	} else {
		*plExisting = plNew;
	}
}

stProcessList *fnStProcessListPopMatching(stProcessList **plProcessList, pid_t pidRm)
{
	/* find the node to be removed and its previous sibling */
	stProcessList *plL = *plProcessList;
	stProcessList *plR = *plProcessList;
	for ( ; plR != NULL && plR->pidSelf != pidRm; plR = plR->prNext) { plL = plR; }

	/* remove it from the linked list if it exists */
	if (plR != NULL) {
		if (plR == *plProcessList) {
			*plProcessList = (*plProcessList)->prNext;
		} else {
			plL->prNext = plR->prNext;
		}
	}

	return plR;
}

void hSIGTSTP(int iSigno)
{
	if (bForegroundOnly) {
		write(STDOUT_FILENO, "\nExiting foreground-only mode\n: ", 33);
	} else {
		write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n: ", 53);
	}

	bForegroundOnly = !bForegroundOnly;
}
