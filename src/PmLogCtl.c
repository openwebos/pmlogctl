/* @@@LICENSE
*
*      Copyright (c) 2007-2012 Hewlett-Packard Development Company, L.P.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


/**
 *************************************************************************
 * @file PmLogCtl.c
 *
 * @brief PmLogCtl implements a simple command line interface that allows
 * developers to dynamically adjust the logging context output levels.
 *
 *************************************************************************
 */


#include "PmLogCtl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/syslog.h>

#include <PmLogLib.h>


/**
 * @brief ParseFacility
 *
 * "user" => LOG_USER, etc.
 * @return true if parsed OK, else false.
 */
bool ParseFacility(const char* facilityStr, int* facilityP)
{
	const int* nP;

	nP = PmLogStringToFacility(facilityStr);
	if (nP != NULL)
	{
		*facilityP = *nP;
		return true;
	}

	*facilityP = -1;
	return false;
}


/**
 * @brief ParseLevel
 *
 * "err" => LOG_ERR, etc.
 * @return true if parsed OK, else false.
 */
bool ParseLevel(const char* levelStr, int* levelP)
{
	const int* nP;

	nP = PmLogStringToLevel(levelStr);
	if (nP != NULL)
	{
		*levelP = *nP;
		return true;
	}

	*levelP = -1;
	return false;
}


/**
 * @brief GetFacilityStr
 *
 * LOG_USER => "user", etc.  NULL if not recognized.
 */
const char* GetFacilityStr(int facility)
{
	const char* facilityStr;

	facilityStr = PmLogFacilityToString(facility);
	return facilityStr;
}


/**
 * @brief GetLevelStr
 *
 * LOG_ERR => "err", etc. NULL if not recognized.
 */
const char* GetLevelStr(int level)
{
	const char* levelStr;

	levelStr = PmLogLevelToString(level);
	return levelStr;
}


/**
 * @brief SuggestHelp
 *
 * Called during command line parsing when a parameter error is
 * detected.
 */
static void SuggestHelp(void)
{
	printf("Use -help for usage information.\n");
}


/**
 * @brief PrvIsWildcardContextName
 */
static bool PrvIsWildcardContextName(const char* matchContextName)
{
	if (strchr(matchContextName, '*') != NULL)
	{
		return true;
	}

	return false;
}


/**
 * @brief PrvMatchContextName
 *
 * Match the context name with the given name string and return
 * true if it matches.
 * If matchContextName is NULL it means to match all.
 */
static bool PrvMatchContextName(const char* contextName,
	const char* matchContextName)
{
	const char* matchWildStr;
	size_t		matchPrefixLen;

	if (matchContextName == NULL)
	{
		return true;
	}

	/* to start, only match one wildcard '*' at the end */

	matchWildStr = strchr(matchContextName, '*');
	if (matchWildStr == NULL)
	{
		/* no wildcard means we need an exact match */
		if (strcmp(contextName, matchContextName) == 0)
		{
			return true;
		}
	}
	else
	{
		/* given a wildcard at the end of the match string,
		 * we just need to match any characters before (if any) */

		matchPrefixLen = matchWildStr - matchContextName;
		if (matchPrefixLen == 0)
		{
			return true;
		}

		if (memcmp(contextName, matchContextName, matchPrefixLen) == 0)
		{
			return true;
		}
	}

	return false;
}


typedef struct
{
	PmLogContext	context;
	char			contextName[ PMLOG_MAX_CONTEXT_NAME_LEN + 1 ];
}
ContextInfo_t;


typedef struct
{
	int				numContexts;
	ContextInfo_t	contextInfos[ PMLOG_MAX_NUM_CONTEXTS + 1];
}
ContextsInfo_t;


/**
 * @brief SortCmpContextInfoByName
 */
static int SortCmpContextInfoByName(const void* p1, const void* p2)
{
	const ContextInfo_t* context1P	= (const ContextInfo_t*) p1;
	const ContextInfo_t* context2P	= (const ContextInfo_t*) p2;

	return strcasecmp(context1P->contextName, context2P->contextName);
}


/**
 * @brief PrvGetContextList
 */
static PmLogErr PrvGetContextList(ContextsInfo_t* contextInfosP,
	const char* matchContextName)
{
	PmLogErr 		logErr;
	int				n;
	int				i;
	ContextInfo_t*	contextInfoP;

	memset(contextInfosP, 0, sizeof(*contextInfosP));

	contextInfosP->numContexts = 0;

	n = 0;
	logErr = PmLogGetNumContexts(&n);
	if (logErr != kPmLogErr_None)
	{
		return logErr;
	}

	if (n <= 0)
	{
		return kPmLogErr_Unknown;
	}

	for (i = 0; i < n; i++)
	{
		contextInfoP = &contextInfosP->contextInfos[ contextInfosP->numContexts ];

		contextInfoP->context = NULL;
		logErr = PmLogGetIndContext(i, &contextInfoP->context);
		if (logErr != kPmLogErr_None)
		{
			return logErr;
		}

		contextInfoP->contextName[ 0 ] = 0;
		logErr = PmLogGetContextName(contextInfoP->context,
			contextInfoP->contextName, sizeof(contextInfoP->contextName));
		if (logErr != kPmLogErr_None)
		{
			return logErr;
		}

		if (!PrvMatchContextName(contextInfoP->contextName, matchContextName))
		{
			continue;
		}

		if (contextInfosP->numContexts >= (PMLOG_MAX_NUM_CONTEXTS + 1))
		{
			return kPmLogErr_Unknown;
		}

		contextInfosP->numContexts++;
	}

	if (contextInfosP->numContexts > 0)
	{
		qsort(&contextInfosP->contextInfos, contextInfosP->numContexts,
			sizeof(ContextInfo_t), SortCmpContextInfoByName);
	}

	return kPmLogErr_None;
}


/**
 * @brief PrvResolveContextNameAlias
 *
 * As a convenience, rather than making the command line user enter
 * "<global>" to refer to the global context, we also accept "." to
 * mean the same.
 */
static const char* PrvResolveContextNameAlias(const char* contextName)
{
	if (strcmp(contextName, ".") == 0)
	{
		return kPmLogGlobalContextName;
	}

	return contextName;
}


/**
 * @brief ShowContext
 *
 * Display information about the given logging context, i.e. name
 * and active level.
 *
 * If matchContextName is specified, only display information if the
 * given context has the same name.
 *
 * @return 1 if matched else 0.
 */
static void ShowContext(const ContextInfo_t* contextInfoP)
{
	const char*	levelStr;

	levelStr = PmLogLevelToString(contextInfoP->context->enabledLevel);
	if (levelStr == NULL)
	{
		levelStr = "Unknown";
	}

	printf("Context '%s' = %s\n", contextInfoP->contextName, levelStr);
}


/**
 * @brief DoCmdShow
 *
 * Usage: show [<context>]             # show logging context(s)
 *
 * By default, show information about all registered logging contexts,
 * else show information for the specified context.
 */
static Result DoCmdShow(int argc, char* argv[])
{
	const char*		matchContextName;
	ContextsInfo_t	contextInfos;
	PmLogErr 		logErr;
	int				i;
	ContextInfo_t*	contextInfoP;

	matchContextName = NULL;

	if (argc >= 2)
	{
		matchContextName = argv[ 1 ];
		matchContextName = PrvResolveContextNameAlias(matchContextName);
	}

	if (argc >= 3)
	{
		printf("Invalid parameter '%s'\n", argv[2]);
		return RESULT_PARAM_ERR;
	}

	logErr = PrvGetContextList(&contextInfos, matchContextName);
	if (logErr != kPmLogErr_None)
	{
		printf("Error getting contexts info: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	for (i = 0; i < contextInfos.numContexts; i++)
	{
		contextInfoP = &contextInfos.contextInfos[ i ];
		ShowContext(contextInfoP);
	}

	if (matchContextName != NULL)
	{
		if (contextInfos.numContexts == 0)
		{
			if (PrvIsWildcardContextName(matchContextName))
			{
				printf("No contexts matched '%s'.\n", matchContextName);
			}
			else
			{
				printf("Context '%s' not found.\n", matchContextName);
			}
			return RESULT_RUN_ERR;
		}
	}

	return RESULT_OK;
}


/**
 * @brief DoCmdSet
 *
 * Usage: set <context> <level>        # set logging context level
 *
 * Set the active logging level for the specified context.
 * If the context does not already exist, it is an error.
 */
static Result DoCmdSet(int argc, char* argv[])
{
	int				i;
	const char*		arg;
	const char*		matchContextName;
	PmLogContext	matchedContext;
	const int*		levelIntP;
	PmLogErr		logErr;
	ContextsInfo_t	contextInfos;
	ContextInfo_t*	contextInfoP;

	matchContextName = NULL;
	matchedContext = NULL;
	levelIntP = NULL;

	i = 1;
	while (i < argc)
	{
		arg = argv[ i ];

		if (matchContextName == NULL)
		{
			matchContextName = arg;
			matchContextName = PrvResolveContextNameAlias(matchContextName);
			if (!PrvIsWildcardContextName(matchContextName))
			{
				logErr = PmLogFindContext(matchContextName, &matchedContext);
				if (logErr != kPmLogErr_None)
				{
					printf("Context '%s' not found.\n", matchContextName);
					return RESULT_PARAM_ERR;
				}
			}
			i++;
		}
		else if (levelIntP == NULL)
		{
			levelIntP = PmLogStringToLevel(arg);
			if (levelIntP == NULL)
			{
				printf("Invalid level '%s'.\n", arg);
				return RESULT_PARAM_ERR;
			}
			i++;
		}
		else
		{
			printf("Invalid parameter '%s'.\n", arg);
			return RESULT_PARAM_ERR;
		}
	}

	if (matchContextName == NULL)
	{
		printf("Context not specified.\n");
		return RESULT_PARAM_ERR;
	}

	if (levelIntP == NULL)
	{
		printf("Level not specified.\n");
		return RESULT_PARAM_ERR;
	}

	if (matchedContext == NULL)
	{
		/* If a specific context wasn't matched, it's a wildcard match */

		logErr = PrvGetContextList(&contextInfos, matchContextName);
		if (logErr != kPmLogErr_None)
		{
			printf("Error getting contexts info: 0x%08X (%s)\n", logErr,
				PmLogGetErrDbgString(logErr));
			return RESULT_RUN_ERR;
		}

		if (contextInfos.numContexts == 0)
		{
			printf("No contexts matched '%s'.\n", matchContextName);
			return RESULT_RUN_ERR;
		}

		for (i = 0; i < contextInfos.numContexts; i++)
		{
			contextInfoP = &contextInfos.contextInfos[ i ];

			printf("Setting context level for '%s'.\n",
				contextInfoP->contextName);

			logErr = PmLogSetContextLevel(contextInfoP->context, *levelIntP);
			if (logErr != kPmLogErr_None)
			{
				printf("Error setting context log level: 0x%08X (%s)\n", logErr,
					PmLogGetErrDbgString(logErr));
				return RESULT_RUN_ERR;
			}
		}
	}
	else
	{
		printf("Setting context level for '%s'.\n", matchContextName);

		logErr = PmLogSetContextLevel(matchedContext, *levelIntP);
		if (logErr != kPmLogErr_None)
		{
			printf("Error setting context log level: 0x%08X (%s)\n", logErr,
				PmLogGetErrDbgString(logErr));
			return RESULT_RUN_ERR;
		}
	}

	return RESULT_OK;
}


/**
 * @brief DoCmdLog
 *
 * Usage: log <context> <level> <msg>  # log a message
 *
 * Test a call through PmLogLib to log a message on the given context
 * with the given level. If the context does not exist it is an error.
 */
static Result DoCmdLog(int argc, char* argv[])
{
	int				i;
	const char*		arg;
	const char*		contextName;
	PmLogContext	context;
	const int*		levelIntP;
	PmLogErr		logErr;
	const char*		msg;
	int				defaultLevel;

	contextName = NULL;
	context = NULL;
	levelIntP = NULL;
	msg = NULL;

	/* if only one parameter was specified, use default context and level */
	if (argc == 2)
	{
		contextName = kPmLogGlobalContextName;
		context = kPmLogGlobalContext;

		defaultLevel = kPmLogLevel_Notice;
		levelIntP = &defaultLevel;
	}

	i = 1;
	while (i < argc)
	{
		arg = argv[ i ];

		if (contextName == NULL)
		{
			contextName = arg;
			contextName = PrvResolveContextNameAlias(contextName);
			logErr = PmLogFindContext(contextName, &context);
			if (logErr != kPmLogErr_None)
			{
				printf("Invalid context '%s'.\n", arg);
				return RESULT_PARAM_ERR;
			}
			i++;
		}
		else if (levelIntP == NULL)
		{
			levelIntP = PmLogStringToLevel(arg);
			if ((levelIntP == NULL) ||
				(*levelIntP == -1))
			{
				printf("Invalid level '%s'.\n", arg);
				return RESULT_PARAM_ERR;
			}
			i++;
		}
		else if (msg == NULL)
		{
			msg = arg;
			i++;
		}
		else
		{
			printf("Invalid parameter '%s'.\n", arg);
			return RESULT_PARAM_ERR;
		}
	}

	if (contextName == NULL)
	{
		printf("Context not specified.\n");
		return RESULT_PARAM_ERR;
	}

	if (levelIntP == NULL)
	{
		printf("Level not specified.\n");
		return RESULT_PARAM_ERR;
	}

	if (msg == NULL)
	{
		printf("Message not specified.\n");
		return RESULT_PARAM_ERR;
	}

	logErr = PmLogPrint_(context, *levelIntP, "%s", msg);
	if (logErr != kPmLogErr_None)
	{
		printf("Error logging: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	return RESULT_OK;
}


/**
 * @brief WriteKMsg
 *
 * Write a kernel message.
 */
static Result WriteKMsg(int priority, const char* msgStr)
{
	const char* kKMsgPath = "/dev/kmsg";

	FILE*	f;
	int		err;
	char	priorityStr[ 8 ];
	int		n;
	Result	result;

	f = fopen(kKMsgPath, "w");
	if (f == NULL)
	{
		err = errno;
		printf("Error opening %s: %s", kKMsgPath, strerror(err));
		return RESULT_RUN_ERR;
	}

	if (priority >= 0)
	{
		mysprintf(priorityStr, sizeof(priorityStr), "<%d>", priority);
	}
	else
	{
		priorityStr[ 0 ] = 0;
	}

	n = fprintf(f, "%s%s\n", priorityStr, msgStr);
	if (n < 0)
	{
		err = errno;
		printf("Error writing %s: %s", kKMsgPath, strerror(err));
		result = RESULT_RUN_ERR;
	}
	else
	{
		result = RESULT_OK;
	}

	(void) fclose(f);

	return result;
}


/**
 * @brief DoCmdKLog
 *
 * Usage: klog [-p <level>] <msg>  # log a message
 *
 * Test a call through printk.
 */
static Result DoCmdKLog(int argc, char* argv[])
{
	int				i;
	const char*		arg;
	int				level;
	const int*		levelIntP;
	const char*		msg;
	Result			result;

	level = kPmLogLevel_Notice;
	levelIntP = NULL;
	msg = NULL;

	i = 1;
	while (i < argc)
	{
		arg = argv[ i ];

		if (arg[ 0 ] == '-')
		{
			if (strcmp(arg, "-p") == 0)
			{
				i++;
				if (i >= argc)
				{
					printf("Invalid parameter: -p requires value\n");
					return RESULT_PARAM_ERR;
				}

				arg = argv[ i ];
				levelIntP = PmLogStringToLevel(arg);
				if (levelIntP == NULL)
				{
					printf("Invalid level '%s'.\n", arg);
					return RESULT_PARAM_ERR;
				}

				level = *levelIntP;
				i++;
			}
			else
			{
				printf("Invalid parameter '%s'.\n", arg);
				return RESULT_PARAM_ERR;
			}
		}
		else if (msg == NULL)
		{
			msg = arg;
			i++;
		}
		else
		{
			printf("Invalid parameter '%s'.\n", arg);
			return RESULT_PARAM_ERR;
		}
	}

	if (msg == NULL)
	{
		printf("Message not specified.\n");
		return RESULT_PARAM_ERR;
	}

	#if 1
	{
		result = WriteKMsg(level, msg);
	}
	#else
	{
		n = klogctl(10, (char*) msg, strlen(msg));
		printf("klogctl = %d\n", n);
		result = RESULT_OK;
	}
	#endif

	return result;
}

/**
 * @brief DoCmdFlush
 *
 * Usage: flush
 */
static Result DoCmdFlush()
{
	PmLogErr		logErr;
	PmLogContext	context;

	logErr = PmLogFindContext("PmLogCtl", &context);
	if (logErr != kPmLogErr_None)
	{
		printf("Error getting context PmLogCtl: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	logErr = PmLogPrint(context, kPmLogLevel_Emergency, "Manually Flushing Buffers");
	if (logErr != kPmLogErr_None)
	{
		printf("Error logging: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	return RESULT_OK;
}


/**
 * @brief DoCmdReConf
 *
 * Usage: reconf
 *
 * Issue the PmLogLib command that forces the global options to be
 * reloaded from /etc/PmLogContexts.conf.
 */
static Result DoCmdReConf(int argc, char* argv[])
{
	int				i;
	const char*		arg;
	PmLogErr		logErr;

	i = 1;
	while (i < argc)
	{
		arg = argv[ i ];

		printf("Invalid parameter '%s'.\n", arg);
		return RESULT_PARAM_ERR;
	}

	logErr = PmLogPrint_(kPmLogGlobalContext, kPmLogLevel_Emergency,
		"!loglib loadconf");
	if (logErr != kPmLogErr_None)
	{
		printf("Error logging: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	return RESULT_OK;
}


/**
 * @brief DoCmdDef
 *
 * Usage: def <context> [<level>]      # define logging context
 *
 * Defines the specified logging context.
 * If the level is not specified it is assigned a default.
 * If the context already exists it is an error.
 */
static Result DoCmdDef(int argc, char* argv[])
{
	const int kLevelNotSpecified = -999;

	int				i;
	const char*		arg;
	const char*		contextName;
	PmLogContext	context;
	const int*		levelIntP;
	int				level;
	PmLogErr		logErr;

	contextName = NULL;
	context = NULL;
	levelIntP = NULL;
	level = kLevelNotSpecified;

	i = 1;
	while (i < argc)
	{
		arg = argv[ i ];

		if (contextName == NULL)
		{
			contextName = arg;
			contextName = PrvResolveContextNameAlias(contextName);
			logErr = PmLogFindContext(contextName, &context);
			if (logErr == kPmLogErr_None)
			{
				printf("Context '%s' is already defined.\n", contextName);
				return RESULT_PARAM_ERR;
			}
			i++;
		}
		else if (levelIntP == NULL)
		{
			levelIntP = PmLogStringToLevel(arg);
			if (levelIntP == NULL)
			{
				printf("Invalid level '%s'.\n", arg);
				return RESULT_PARAM_ERR;
			}
			level = *levelIntP;
			i++;
		}
		else
		{
			printf("Invalid parameter '%s'.\n", arg);
			return RESULT_PARAM_ERR;
		}
	}

	if (contextName == NULL)
	{
		printf("Context not specified.\n");
		return RESULT_PARAM_ERR;
	}

	logErr = PmLogGetContext(contextName, &context);
	if (logErr != kPmLogErr_None)
	{
		printf("Error defining context: 0x%08X (%s)\n", logErr,
			PmLogGetErrDbgString(logErr));
		return RESULT_RUN_ERR;
	}

	if (level != kLevelNotSpecified)
	{
		logErr = PmLogSetContextLevel(context, level);
		if (logErr != kPmLogErr_None)
		{
			printf("Error setting context log level: 0x%08X (%s)\n", logErr,
				PmLogGetErrDbgString(logErr));
			return RESULT_RUN_ERR;
		}
	}

	return RESULT_OK;
}


/**
 * @brief ShowUsage
 *
 * Print out the command line usage info.
 */
static void ShowUsage(void)
{
	int	level;

	printf("PmLogCtl COMMAND [PARAM...]\n");
	printf("  help                         # show usage info\n");
	printf("  def <context> [<level>]      # define logging context\n");
	printf("  flush                        # flush all ring buffers\n");
	printf("  log <context> <level> <msg>  # log a message\n");
	printf("  klog [-p <level>] <msg>      # log a kernel message\n");
	printf("  reconf                       # re-load lib options from conf\n");
	printf("  set <context> <level>        # set logging context level\n");
	printf("  show [<context>]             # show logging context(s)\n");
	printf("\n");

	printf("Contexts:\n");
	printf("  The global context can be specified as '.'\n");

	printf("\n");

	printf("Levels:\n");
	for (level = -1 /* kPmLogLevel_None */; level <= 7; level++)
	{
		printf("  %-10s  # %d\n", PmLogLevelToString(level), level);
	}
}


/**
 * @brief main
 */
int main(int argc, char* argv[])
{
	Result	result;
	char*	cmd;

	do
	{
		if (argc < 2)
		{
			printf("No command specified.\n");
			result = RESULT_PARAM_ERR;
			break;
		}

		cmd = argv[ 1 ];

		if (strcmp(cmd, "def") == 0)
		{
			result = DoCmdDef(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "log") == 0)
		{
			result = DoCmdLog(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "klog") == 0)
		{
			result = DoCmdKLog(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "reconf") == 0)
		{
			result = DoCmdReConf(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "set") == 0)
		{
			result = DoCmdSet(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "show") == 0)
		{
			result = DoCmdShow(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "view") == 0)
		{
			result = DoCmdView(argc - 1, &argv[1]);
		}
		else if (strcmp(cmd, "flush") == 0)
		{
			result = DoCmdFlush();
		}
		else if ((strcmp(cmd, "help") == 0) ||
			(strcmp(cmd, "-help") == 0))
		{
			ShowUsage();
			result = RESULT_HELP;
		}
		else
		{
			printf("Invalid command '%s'\n", cmd);
			result = RESULT_PARAM_ERR;
		}
	}
	while (false);

	if (result == RESULT_PARAM_ERR)
	{
		SuggestHelp();
	}

	exit((result == RESULT_OK) ? EXIT_SUCCESS : EXIT_FAILURE);
}
