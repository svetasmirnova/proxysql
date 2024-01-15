#include <vector>
#include <string>
#include <stdio.h>
#include <cstring>
#include <unistd.h>
#include "mysql.h"

#include "tap.h"
#include "command_line.h"
#include "utils.h"


/*
This test verifies that client connections are dropped because of:
- mysql-wait_timeout
- mysql-max_transaction_time
*/

const int NUM_CONNS = 5;

int run_q(MYSQL *mysql, const char *q) {
	MYSQL_QUERY(mysql,q);
	return 0;
}

MYSQL * conns[NUM_CONNS];
unsigned long mythreadid[NUM_CONNS];

int create_connections(CommandLine& cl) {
	for (int i = 0; i < NUM_CONNS ; i++) {

		MYSQL * mysql = mysql_init(NULL);
		diag("Connecting: cl.username='%s' cl.use_ssl=%d", cl.username, cl.use_ssl);
		if (cl.use_ssl)
			mysql_ssl_set(mysql, NULL, NULL, NULL, NULL, NULL);
		if (!mysql_real_connect(mysql, cl.host, cl.username, cl.password, NULL, cl.port, NULL, 0)) {
			fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(mysql));
			return exit_status();
		} else {
			const char * c = mysql_get_ssl_cipher(mysql);
			ok(cl.use_ssl == 0 ? c == NULL : c != NULL, "Cipher: %s", c == NULL ? "NULL" : c);
			ok(mysql->net.compress == 0, "Compression: (%d)", mysql->net.compress);
		}

		conns[i] = mysql;
	}
	return 0;
}

int find_tids() {
	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		unsigned long tid;
		MYSQL_ROW row;
		MYSQL_QUERY(mysql, "SELECT CONNECTION_ID()");
		MYSQL_RES * proxy_res = mysql_store_result(mysql);
		while ((row = mysql_fetch_row(proxy_res))) {
			tid = atoll(row[0]);
		}
		mysql_free_result(proxy_res);
		ok(tid == mysql_thread_id(mysql), "tid: %lu, mysql_thread_id(): %lu", tid, mysql_thread_id(mysql));
		mythreadid[i] = tid;
	}

	return 0;
}

int main(int argc, char** argv) {
	CommandLine cl;

	int np = 2;
	np += 2 * NUM_CONNS ;	// connection
	np += NUM_CONNS ;	// to get connection id
	np += NUM_CONNS ;	// failed query on killed connection due to timeout
	np += 2 * NUM_CONNS ;	// connection
	np += NUM_CONNS ;	// to get connection id
	np += NUM_CONNS ;	// to run BEGIN
	np += NUM_CONNS ;	// to run first DO 1
	np += NUM_CONNS ;	// failed query on killed connection due transaction timeout

	plan(np);


	MYSQL* proxysql_admin = mysql_init(NULL);
	diag("Connecting: cl.admin_username='%s' cl.use_ssl=%d", cl.admin_username, cl.use_ssl);
	if (cl.use_ssl)
		mysql_ssl_set(proxysql_admin, NULL, NULL, NULL, NULL, NULL);
	if (!mysql_real_connect(proxysql_admin, cl.host, cl.admin_username, cl.admin_password, NULL, cl.admin_port, NULL, 0)) {
		fprintf(stderr, "File %s, line %d, Error: %s\n", __FILE__, __LINE__, mysql_error(proxysql_admin));
		return -1;
	} else {
		const char * c = mysql_get_ssl_cipher(proxysql_admin);
		ok(cl.use_ssl == 0 ? c == NULL : c != NULL, "Cipher: %s", c == NULL ? "NULL" : c);
		ok(proxysql_admin->net.compress == 0, "Compression: (%d)", proxysql_admin->net.compress);
	}

	MYSQL_QUERY(proxysql_admin, "SET mysql-have_ssl='true'");
	MYSQL_QUERY(proxysql_admin, "SET mysql-have_compress='true'");
	MYSQL_QUERY(proxysql_admin, "SET mysql-auto_increment_delay_multiplex=10"); // to ensure multiplexing stays disabled
	MYSQL_QUERY(proxysql_admin, "SET mysql-wait_timeout=6000"); // to force a close
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");

	int rc = 0;
	rc = create_connections(cl);
	if (rc != 0) {
		return exit_status();
	}

	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		if (i == 0) {
			if (create_table_test_sbtest1(10,mysql)) {
				fprintf(stderr, "File %s, line %d, Error: create_table_test_sbtest1() failed\n", __FILE__, __LINE__);
				return exit_status();
			}
		} else {
			if (add_more_rows_test_sbtest1(10,mysql)) {
				fprintf(stderr, "File %s, line %d, Error: add_more_rows_sbtest1() failed\n", __FILE__, __LINE__);
				return exit_status();
			}
		}
	}

	rc = find_tids();
	if (rc != 0) {
		return exit_status();
	}

	diag("Sleeping for 10 seconds");
	for (int i = 0; i < 10 ; i++) {
		fprintf(stderr,".");
		sleep(1);
	}
	fprintf(stderr,"\n");

	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		int rc = run_q(mysql, "DO 1");
		ok(rc != 0, (rc == 0 ? "Connection still alive" : "Connection killed"));
	}

	MYSQL_QUERY(proxysql_admin, "SET mysql-wait_timeout=3600000"); // to not force a close on wait
	MYSQL_QUERY(proxysql_admin, "SET mysql-max_transaction_time=17000"); // to force a close on transaction	
	MYSQL_QUERY(proxysql_admin, "LOAD MYSQL VARIABLES TO RUNTIME");

	rc = create_connections(cl);
	if (rc != 0) {
		return exit_status();
	}

	rc = find_tids();
	if (rc != 0) {
		return exit_status();
	}

	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		int rc = run_q(mysql, "BEGIN");
		ok(rc == 0, "Running BEGIN on new connection");
	}

	diag("Sleeping for 12 seconds");
	for (int i = 0; i < 12 ; i++) {
		fprintf(stderr,".");
		sleep(1);
	}
	fprintf(stderr,"\n");

	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		int rc = run_q(mysql, "DO 1");
		ok(rc == 0, (rc == 0 ? "Connection still alive" : "Connection killed"));
	}

	diag("Sleeping for 12 seconds");
	for (int i = 0; i < 12 ; i++) {
		fprintf(stderr,".");
		sleep(1);
	}
	fprintf(stderr,"\n");

	for (int i = 0; i < NUM_CONNS ; i++) {
		MYSQL * mysql = conns[i];
		int rc = run_q(mysql, "DO 1");
		ok(rc != 0, (rc == 0 ? "Connection still alive" : "Connection killed"));
	}

	return exit_status();
}
