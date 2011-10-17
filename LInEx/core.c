/*
 * LInEx - Lightweight Information Export
 * Copyright (C) 2010 Vermont Project (http://vermont.berlios.de)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

/*
 * core.c
 *
 *  Created on: 22.11.2009
 *      Author: kami
 */

//#include "core.h"
#include "config_file.h"
#include "ipfixlolib/msg.h"
#include "ipfix_templates.h"
#include "ipfix_data.h"
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include "flows/flows.h"
#include "flows/olsr.h"
#include "flows/topology_set.h"
#include "flows/hello_set.h"
#include "flows/export.h"
#include "event_loop.h"

struct export_record_parameters {
	ipfix_exporter *exporter;
	config_file_descriptor* conf;
	FILE *xmlfh;
};

void export_records(struct export_record_parameters *params);

regex_t param_regex;
regex_t long_param_regex;
regmatch_t param_matches[3];
char* config_file = NULL;
pid_t childpid = -1;
extern node_set_hash *node_set;

/**
 * Takes all collectors from config file <conf>
 * and adds them to the exporter <exporter>
 * by calling the appropriate ipfixlolib function.
 */
void init_collectors(config_file_descriptor* conf, ipfix_exporter* exporter){
	list_node* cur;

	ipfix_aux_config_udp aux_config; /* Auxiliary parameter for UDP */
	aux_config.mtu = 1500;           /* MTU */
	for(cur = conf->collectors->first;cur!=NULL;cur=cur->next){
		collector_descriptor* cur_descriptor = (collector_descriptor*)cur->data;
		int ret = ipfix_add_collector(exporter, cur_descriptor->ip, cur_descriptor->port, cur_descriptor->transport_protocol, &aux_config);
		msg(MSG_INFO, "Added collector %s:%d (return: %d)", cur_descriptor->ip,cur_descriptor->port,  ret);
	}
}

void usage(){
	printf("Usage: LInEx -f <config_file> [-v <X>]\n");
	printf("-f <config_file>     specifies configuration file\n");
	printf("-v <X>               sets verbosity level (X=0,1,2,3,4,5 default=2)\n");
}

void parse_command_line_parameters(int argc, char **argv){
	/* parse command line */
	int c;
	while ((c=getopt(argc, argv, "hf:v:")) != -1) {

		switch (c) {

			case 'f':
				config_file = optarg;
				break;

			case 'v':
				msg_setlevel(atoi(optarg));
				break;

			case 'h':
			default:
				/* print usage and quit vermont, if unknow switch */
				usage();
				exit(1);
		}
	}
	if (config_file == NULL)
	{
		usage();
		exit(1);
	}
}

void sigwait_handler(int signal)
{
	int status;
	pid_t pid;

        //Wait for child without blocking
        if ((pid = waitpid(-1, &status, WNOHANG)) < 0) 
	{
		//msg(MSG_VDEBUG, "waitpid failed.");
		return;
	}
	//Return if this is not the XML postprocessing child
	if (pid != childpid) return; 

	childpid = -1;
}

/**
 * Test main methode
 */
int main(int argc, char **argv)
{
	//Initialize signal handler
	struct sigaction new_sa;
	new_sa.sa_handler = sigwait_handler;
	sigemptyset(&new_sa.sa_mask);
	new_sa.sa_flags = 0;
	if (sigaction(SIGCHLD, &new_sa, NULL) == -1) {
		THROWEXCEPTION("Could not install signal handler.");
	}

	//Process command line parameters
	parse_command_line_parameters(argc,argv);

	//Read config file
	config_file_descriptor* conf = read_config(config_file);

	//Init exporter
	ipfix_exporter* send_exporter;
	int ret = ipfix_init_exporter(conf->observation_domain_id, &send_exporter);
	if (ret != 0) {
		THROWEXCEPTION("ipfix_init_exporter failed!\n");
	}
#ifdef SUPPORT_COMPRESSION
	if (conf->compression_method && strlen(conf->compression_method) > 0) {
		ret = ipfix_init_compression(send_exporter,
									 conf->compression_method,
									 conf->compression_method_params);
		if (ret)
			THROWEXCEPTION("Failed to initialize compression module.");
	}
#endif

	//Add collectors from config file
	init_collectors(conf,send_exporter);

	//Generate templates
	msg(MSG_INFO, "Generating templates from config");
	generate_templates_from_config(send_exporter,conf);
	msg(MSG_DIALOG, "LInEx is up and running. Press Ctrl-C to exit.");

	// Start capturing sessions
	flow_capture_session session;

	if (start_flow_capture_session(&session, 30))
		msg(MSG_ERROR, "Failed to start capture session.");
	else {
		struct lnode *node = conf->interfaces->first;

		while (node != NULL) {
			char *interface = (char *) node->data;
			DPRINTF("Adding interface %s to capture session.", interface);
			if (add_interface(&session, interface, 1))
				msg(MSG_ERROR, "Failed to add interface %s to capture session.", interface);

			node = node->next;
		}
	}

	struct capture_session *olsr_capture_session = start_capture_session();
	if (!olsr_capture_session) {
		msg(MSG_ERROR, "Failed to start OLSR capture session.");
	} else {
		struct lnode *node = conf->interfaces->first;

		while (node != NULL) {
			char *interface = (char *) node->data;
			DPRINTF("Adding interface %s to capture session.", interface);

			if (!olsr_add_capture_interface(olsr_capture_session, interface))
				msg(MSG_ERROR, "Failed to add OLSR capturing to interface %s.", interface);
			node = node->next;
		}
	}

#ifdef SUPPORT_ANONYMIZATION
	if (init_cryptopan(&session.cryptopan,
					   conf->anonymization_key,
					   conf->anonymization_pad)) {
		msg(MSG_ERROR, "Failed to initialize CryptoPAN.");
		return 1;
	}
#endif

	// Declare IPFIX templates to export monitoring information
	if (declare_templates(send_exporter))
		msg(MSG_ERROR, "Failed to export templates.");

	//Open XML file
	FILE* xmlfh = NULL;
	if(conf->xmlfile != NULL) {
		xmlfh = fopen(conf->xmlfile, "w");
		if (xmlfh == NULL)
			THROWEXCEPTION("Could not open XML file %s", conf->xmlfile);
	}

	// Add timer to export routing tables
	node_set = kh_init(2);

	struct export_parameters params = { send_exporter, node_set };
	event_loop_add_timer(20000, (void (*)(void *)) &export_full, &params);

	// Add timer to export flows
	struct export_flow_parameter flow_param = { send_exporter, &session };
	event_loop_add_timer(5000, (void (*)(void *)) &export_flows, &flow_param);

	// Add timer to export records
	struct export_record_parameters record_params = { send_exporter, conf, xmlfh };
	event_loop_add_timer(conf->interval * 1000, (void (*)(void *)) &export_records, &record_params);

	// Add timer to export capture statistics
	struct export_capture_parameter capture_statistics_param = {
		send_exporter,
		session.capture_session,
		olsr_capture_session
	};
	event_loop_add_timer(10000, (void (*) (void *)) &export_capture_statistics, &capture_statistics_param);

	return event_loop_run();
}

void export_records(struct export_record_parameters *params) {
	time_t now = time(NULL);
	char timestr[20];
	ipfix_exporter *send_exporter = params->exporter;
	config_file_descriptor* conf = params->conf;
	FILE *xmlfh = params->xmlfh;

	strftime(timestr, 20, "%X", localtime(&now));

	msg(MSG_DIALOG, "Export status at %s", timestr);

	if(conf->record_descriptors->size>0 && conf->collectors->size>0) {
		msg(MSG_INFO, "Exporting IPFIX messages...");

		config_to_ipfix(send_exporter, conf);
	}

	if(xmlfh != NULL) {
		msg(MSG_INFO, "Updating XML file %s", conf->xmlfile);
		config_to_xml(xmlfh, conf);
		//Optional XML postprocessing
		if(conf->xmlpostprocessing != NULL) {
			//Kill old XML postprocessing child if it is still alive
			if(childpid != -1) {
				msg(MSG_FATAL, "XML postprocessing has not terminated in time. Killing it.");
				kill(childpid, SIGKILL);
			}
			//Create new XML postprocessing child
			if((childpid = fork()) == -1) {
				msg(MSG_FATAL, "Could not fork. XML postprocessing skipped.");
			}
			if(childpid == 0) {
				msg(MSG_INFO, "Trigger XML postprocessing.");
				int ret = system(conf->xmlpostprocessing);
				exit(ret);
			}
		}
	}

}



