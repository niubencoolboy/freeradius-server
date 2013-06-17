/*
 * sql_freetds.c	freetds (ctlibrary) routines for rlm_sql
 *		Error handling stolen from freetds example code "firstapp.c"
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 * Copyright 2000  Mattias Sjostrom <mattias@nogui.se>
 */

RCSID("$Id$")

#include <freeradius-devel/radiusd.h>

#include <sys/stat.h>

#include <ctpublic.h>

#include "rlm_sql.h"


typedef struct rlm_sql_freetds_conn {
	CS_CONTEXT	*context;
	CS_CONNECTION	*db;
	CS_COMMAND	*command;
	char		**results;
	int		id;
	int		in_use;
	struct timeval	tv;
	char		*error;
} rlm_sql_freetds_conn_t;


#define	MAX_DATASTR_LEN	256

/************************************************************************
*  Client-Library error handler.
************************************************************************/

static CS_RETCODE CS_PUBLIC clientmsg_callback(CS_CONTEXT *context, UNUSED CS_CONNECTION *conn, CS_CLIENTMSG *emsgp)
{
	rlm_sql_freetds_conn_t *this = NULL;
	int len;
		
	if ((cs_config(context, CS_GET, CS_USERDATA, &this, sizeof(this), &len) != CS_SUCCEED) && !this) {
		ERROR("rlm_sql_freetds: failed retrieving context userdata");
		
		return CS_SUCCEED;
	}
	
	if (this->error) TALLOC_FREE(this->error);
	
	this->error = talloc_asprintf(this, "client error: severity(%ld), number(%ld), origin(%ld), layer(%ld): %s", 
				      (long)CS_SEVERITY(emsgp->severity), (long)CS_NUMBER(emsgp->msgnumber),
				      (long)CS_ORIGIN(emsgp->msgnumber), (long)CS_LAYER(emsgp->msgnumber),
				      emsgp->msgstring);
				     
	if (emsgp->osstringlen > 0) {
		this->error = talloc_asprintf_append(this->error, ". os error: number(%ld): %s",
						     (long)emsgp->osnumber, emsgp->osstring);
	}
		
	return CS_SUCCEED;
}

/************************************************************************
*  CS-Library error handler. This function will be invoked
*  when CS-Library has detected an error.
************************************************************************/
static CS_RETCODE CS_PUBLIC csmsg_callback(CS_CONTEXT *context, CS_CLIENTMSG *emsgp)
{
	rlm_sql_freetds_conn_t *this = NULL;
	int len;
	
	if ((cs_config(context, CS_GET, CS_USERDATA, &this, sizeof(this), &len) != CS_SUCCEED) && !this) {
		ERROR("rlm_sql_freetds: failed retrieving context userdata");
		
		return CS_SUCCEED;
	}
	
	if (this->error) TALLOC_FREE(this->error);
	
	this->error = talloc_asprintf(this, "cs error: severity(%ld), number(%ld), origin(%ld), layer(%ld): %s", 
				      (long)CS_SEVERITY(emsgp->severity), (long)CS_NUMBER(emsgp->msgnumber),
				      (long)CS_ORIGIN(emsgp->msgnumber), (long)CS_LAYER(emsgp->msgnumber),
				      emsgp->msgstring);
				     
	if (emsgp->osstringlen > 0) {
		this->error = talloc_asprintf_append(this->error, ". os error: number(%ld): %s",
						     (long)emsgp->osnumber, emsgp->osstring);
	}

	return CS_SUCCEED;
}

/************************************************************************
* Handler for server messages. Client-Library will call this
* routine when it receives a message from the server.
************************************************************************/
static CS_RETCODE CS_PUBLIC servermsg_callback(CS_CONTEXT *context, UNUSED CS_CONNECTION *conn, CS_SERVERMSG *msgp)
{
	rlm_sql_freetds_conn_t *this = NULL;
	int len;
	
	if ((cs_config(context, CS_GET, CS_USERDATA, &this, sizeof(this), &len) != CS_SUCCEED) && !this) {
		ERROR("rlm_sql_freetds: failed retrieving context userdata");
		
		return CS_SUCCEED;
	}
	
	if (this->error) TALLOC_FREE(this->error);
	
	this->error = talloc_asprintf(this, "server error: severity(%ld), number(%ld), origin(%ld), layer(%ld): %s",
				      (long)msgp->msgnumber, (long)msgp->severity,
				      (long)msgp->state, (long)msgp->line, msgp->text);

	/*
	 *	Print the server and procedure names if supplied.
	 */
	if (msgp->svrnlen > 0 && msgp->proclen > 0) {
		this->error = talloc_asprintf_append(this->error, ". server name: %s, procedure name: %s",
						     msgp->svrname, msgp->proc);
	}
	
	return CS_SUCCEED;
}

static int sql_socket_destructor(void *c)
{
	rlm_sql_freetds_conn_t *conn = c;
	
	DEBUG2("rlm_sql_freetds: Socket destructor called, closing socket");
	
	if (conn->db) {
		ct_close(conn->db, CS_FORCE_CLOSE);
	}
	
	return RLM_SQL_OK;
}

/*************************************************************************
 *
 *	Function: sql_socket_init
 *
 *	Purpose: Establish db to the db
 *
 *************************************************************************/
static sql_rcode_t sql_socket_init(rlm_sql_handle_t *handle, rlm_sql_config_t *config) {

	rlm_sql_freetds_conn_t *conn;

	MEM(conn = handle->conn = talloc_zero(handle, rlm_sql_freetds_conn_t));
	talloc_set_destructor((void *) conn, sql_socket_destructor);

	/*
	 *	Allocate a CS context structure. This should really only be done once, but because of
	 *	the db pooling design of rlm_sql, we'll have to go with one context per db
	 */
	if (cs_ctx_alloc(CS_VERSION_100, &conn->context) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to allocate CS context structure (cs_ctx_alloc())");

		goto error;
	}

	/*
	 *	Initialize ctlib
	 */
	if (ct_init(conn->context, CS_VERSION_100) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to initialize Client-Library");

		goto error;
	}

	/*
	 *	Install callback functions for error-handling
	 */
	if (cs_config(conn->context, CS_SET, CS_MESSAGE_CB, (CS_VOID *)csmsg_callback, CS_UNUSED, NULL) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to install CS Library error callback");

		goto error;
	}
	
	if (cs_config(conn->context, CS_SET, CS_USERDATA,
		      (CS_VOID *)&handle->conn, sizeof(handle->conn), NULL) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to set userdata pointer");
		
		goto error;
	}
	
	if (ct_callback(conn->context, NULL, CS_SET, CS_CLIENTMSG_CB, (CS_VOID *)clientmsg_callback) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to install client message callback");

		goto error;
	}

	if (ct_callback(conn->context, NULL, CS_SET, CS_SERVERMSG_CB, (CS_VOID *)servermsg_callback) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to install server message callback");

		goto error;
	}

	/*
	 *	Allocate a ctlib db structure 
	 */
	if (ct_con_alloc(conn->context, &conn->db) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: Unable to allocate db structure");

		goto error;
	}

	/*
	 *	Set User and Password properties for the db
	 */
	{
		CS_VOID *login, *password;
		CS_CHAR *server;
		
		memcpy(&login, &config->sql_login, sizeof(login));
		if (ct_con_props(conn->db, CS_SET, CS_USERNAME, login, strlen(config->sql_login), NULL) != CS_SUCCEED) {
			ERROR("rlm_sql_freetds: unable to set username for db");

			goto error;
		}
		
		memcpy(&password, &config->sql_password, sizeof(password));
		if (ct_con_props(conn->db, CS_SET, CS_PASSWORD,
				 password, strlen(config->sql_password), NULL) != CS_SUCCEED) {
			ERROR("rlm_sql_freetds: unable to set password for db");

			goto error;
		}
		
		/*
		 *	Connect to the database
		 */
		memcpy(&server, &config->sql_server, sizeof(server));
		if (ct_connect(conn->db, server, strlen(config->sql_server)) != CS_SUCCEED) {
			ERROR("rlm_sql_freetds: unable to establish db to symbolic servername %s",
			      config->sql_server);
			
			goto error;
		}
	}

	return RLM_SQL_OK;
	
	error:
	if (conn->context) {
		ct_exit(conn->context, CS_FORCE_EXIT);
		cs_ctx_drop(conn->context);
	}

	return RLM_SQL_ERROR;
}

/*************************************************************************
 *
 *	Function: sql_query
 *
 *	Purpose: Issue a non-SELECT query (ie: update/delete/insert) to
 *	       the database.
 *
 *************************************************************************/
static sql_rcode_t sql_query(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config, char const *query) {

	rlm_sql_freetds_conn_t *conn = handle->conn;

	CS_RETCODE	ret, results_ret;
	CS_INT		result_type;

	if (ct_cmd_alloc(conn->db, &conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to allocate command structure (ct_cmd_alloc())");
		      
		return RLM_SQL_ERROR;
	}

	if (ct_command(conn->command, CS_LANG_CMD, query, CS_NULLTERM, CS_UNUSED) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to initiate command structure (ct_command())");
		      
		return RLM_SQL_ERROR;
	}

	if (ct_send(conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to send command (ct_send())");
		return RLM_SQL_ERROR;
	}

	/*
	 *	We'll make three calls to ct_results, first to get a success indicator, secondly to get a 
	 *	done indicator, and thirdly to get a "nothing left to handle" status.
	 */

	/*
	 *	First call to ct_results, we need returncode CS_SUCCEED and result_type CS_CMD_SUCCEED.
	 */
	if ((results_ret = ct_results(conn->command, &result_type)) == CS_SUCCEED) {
		if (result_type != CS_CMD_SUCCEED) {
			if  (result_type == CS_ROW_RESULT) {
				ERROR("rlm_sql_freetds: sql_query processed a query returning rows. "
				      "Use sql_select_query instead!");
			}
			ERROR("rlm_sql_freetds: result failure or unexpected result type from query");
			
			return RLM_SQL_ERROR;
		}
	} else {
		switch (results_ret) {
		case CS_FAIL: /* Serious failure, freetds requires us to cancel and maybe even close db */
			ERROR("rlm_sql_freetds: Failure retrieving query results");
			      
			if ((ret = ct_cancel(NULL, conn->command, CS_CANCEL_ALL)) == CS_FAIL) {
				INFO("rlm_sql_freetds: cleaning up");

				return RLM_SQL_RECONNECT;
			}
			
			return RLM_SQL_ERROR;
		default:
			ERROR("rlm_sql_freetds: Unexpected return value from ct_results()");
			
			return RLM_SQL_ERROR;
		}
	}

	/*
	 *	Second call to ct_results, we need returncode CS_SUCCEED
	 *	and result_type CS_CMD_DONE.
	 */
	if ((results_ret = ct_results(conn->command, &result_type)) == CS_SUCCEED) {
		if (result_type != CS_CMD_DONE) {
			ERROR("rlm_sql_freetds: Result failure or unexpected result type from query");
			
			return RLM_SQL_ERROR;
		}
	} else {
		switch (results_ret) {
		case CS_FAIL: /* Serious failure, freetds requires us to cancel and maybe even close db */
			ERROR("rlm_sql_freetds: Failure retrieving query results");
			if ((ret = ct_cancel(NULL, conn->command, CS_CANCEL_ALL)) == CS_FAIL) {
				ERROR("rlm_sql_freetds: cleaning up");
				
				return RLM_SQL_RECONNECT;
			}
			return RLM_SQL_ERROR;
			
		default:
			ERROR("rlm_sql_freetds: Unexpected return value from ct_results()");
			
			return RLM_SQL_ERROR;
		}
	}

	/*
	 *	Third call to ct_results, we need returncode CS_END_RESULTS result_type will be ignored.
	 */
	results_ret = ct_results(conn->command, &result_type);
	switch (results_ret) {
	case CS_FAIL: /* Serious failure, freetds requires us to cancel and maybe even close db */
		ERROR("rlm_sql_freetds: Failure retrieving query results");
		if ((ret = ct_cancel(NULL, conn->command, CS_CANCEL_ALL)) == CS_FAIL) {
			INFO("rlm_sql_freetds: cleaning up.");
	
			return RLM_SQL_RECONNECT;
		}
		return RLM_SQL_ERROR;

	case CS_END_RESULTS:  /* This is where we want to end up */
		break;

	default:
		ERROR("rlm_sql_freetds: Unexpected return value from ct_results()");
		
		return RLM_SQL_ERROR;
	}
	return RLM_SQL_OK;
}

/*************************************************************************
 *
 *	Function: sql_num_fields
 *
 *	Purpose: database specific num_fields function. Returns number
 *	       of columns from query
 *
 *************************************************************************/
static int sql_num_fields(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {

	rlm_sql_freetds_conn_t *conn = handle->conn;
	int num;

	if (ct_res_info(conn->command, CS_NUMDATA, (CS_INT *)&num, CS_UNUSED, NULL) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: error retrieving column count");
		
		return RLM_SQL_ERROR;
	}
	return num;
}

/*************************************************************************
 *
 *	Function: sql_error
 *
 *	Purpose: database specific error. Returns error associated with
 *	       connection
 *
 *************************************************************************/
static char const *sql_error(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config)
{
	rlm_sql_freetds_conn_t *conn = handle->conn;

	if (!conn || !conn->db) {
		return "rlm_sql_freetds: no connection to db";
	}
	
	return conn->error;
}

/*************************************************************************
 *
 *	Function: sql_finish_select_query
 *
 *	Purpose: End the select query, such as freeing memory or result
 *
 *************************************************************************/
static sql_rcode_t sql_finish_select_query(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {

	rlm_sql_freetds_conn_t *conn = handle->conn;
	int	i=0;

	ct_cancel(NULL, conn->command, CS_CANCEL_ALL);

	if (ct_cmd_drop(conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: freeing command structure failed");
		
		return RLM_SQL_ERROR;
	}

	if (conn->results) {
		while(conn->results[i]) free(conn->results[i++]);
		free(conn->results);
		conn->results = NULL;
	}

	return RLM_SQL_OK;

}

/*************************************************************************
 *
 *	Function: sql_select_query
 *
 *	Purpose: Issue a select query to the database
 *
 *	Note: Only the first row from queries returning several rows
 *	      will be returned by this function, consequitive rows will
 *	      be discarded.
 *
 *************************************************************************/
static sql_rcode_t sql_select_query(rlm_sql_handle_t *handle, rlm_sql_config_t *config, char const *query) {

	rlm_sql_freetds_conn_t *conn = handle->conn;

	CS_RETCODE	ret, results_ret;
	CS_INT		result_type;
	CS_DATAFMT	descriptor;

	int		colcount,i;
	char		**rowdata;

	 if (!conn->db) {
		ERROR("Socket not connected");
		
		return RLM_SQL_ERROR;
	}

	if (ct_cmd_alloc(conn->db, &conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: unable to allocate command structure (ct_cmd_alloc())");
		      
		return RLM_SQL_ERROR;
	}

	if (ct_command(conn->command, CS_LANG_CMD, query, CS_NULLTERM, CS_UNUSED) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: Unable to initiate command structure (ct_command()");
		      
		return RLM_SQL_ERROR;
	}

	if (ct_send(conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: Unable to send command (ct_send())");
		return RLM_SQL_ERROR;
	}

	results_ret = ct_results(conn->command, &result_type);
	switch (results_ret) {
	case CS_SUCCEED:
		switch (result_type) {
		case CS_ROW_RESULT:

			/*
			 * 	Set up a target buffer for the results data, and associate the buffer with the results,
			 *	but the actual fetching takes place in sql_fetch_row.
			 *	The layer above MUST call sql_fetch_row and/or sql_finish_select_query
			 *	or this socket will be unusable and may cause segfaults
			 *	if reused later on.
			 */

			/*
			 *	Set up the DATAFMT structure that describes our target array
			 *	and tells freetds what we want future ct_fetch calls to do.
			 */
			descriptor.datatype = CS_CHAR_TYPE; 	/* The target buffer is a string */
			descriptor.format = CS_FMT_NULLTERM;	/* Null termination please */
			descriptor.maxlength = MAX_DATASTR_LEN;	/* The string arrays are this large */
			descriptor.count = 1;			/* Fetch one row of data */
			descriptor.locale = NULL;		/* Don't do NLS stuff */

			colcount = sql_num_fields(handle, config); /* Get number of elements in row result */
			
			rowdata = (char **)rad_malloc(sizeof(char *) * (colcount + 1));	/* Space for pointers */
			memset(rowdata, 0, (sizeof(char *) * colcount + 1));  /* NULL-pad the pointers */

			for (i = 0; i < colcount; i++) {
				 /* Space to hold the result data */
				rowdata[i] = rad_malloc((MAX_DATASTR_LEN * sizeof(char))+1);

				/* Associate the target buffer with the data */
				if (ct_bind(conn->command, i+1, &descriptor, rowdata[i], NULL, NULL) != CS_SUCCEED) {
					int j;

					for (j = 0; j <= i; j++) {
						free(rowdata[j]);
					}
					free(rowdata);
					ERROR("rlm_sql_freetds: ct_bind() failed)");
					return RLM_SQL_ERROR;
				}

			}
			
			rowdata[i] = NULL; /* Terminate the array */
			conn->results = rowdata;
			break;

		case CS_CMD_SUCCEED:
		case CS_CMD_DONE:
			ERROR("rlm_sql_freetds: Query returned no data");
			break;

		default:

			ERROR("rlm_sql_freetds: Unexpected result type from query");
			sql_finish_select_query(handle, config);
			
			return RLM_SQL_ERROR;
		}
		break;

	case CS_FAIL:

		/*
		 * Serious failure, freetds requires us to cancel the results and maybe even close the db.
		 */

		ERROR("rlm_sql_freetds: Failure retrieving query results");
		      
		if ((ret = ct_cancel(NULL, conn->command, CS_CANCEL_ALL)) == CS_FAIL) {
			ERROR("rlm_sql_freetds: cleaning up.");

			return RLM_SQL_RECONNECT;
		}
		
		return RLM_SQL_ERROR;

	default:
		ERROR("rlm_sql_freetds: Unexpected return value from ct_results()");
		
		return RLM_SQL_ERROR;
	}
	
	return 0;
}


/*************************************************************************
 *
 *	Function: sql_store_result
 *
 *	Purpose: database specific store_result function. Returns a result
 *	       set for the query.
 *
 *************************************************************************/
static sql_rcode_t sql_store_result(UNUSED rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {
	/*
	 *	Not needed for freetds, code that may have gone here iS in sql_select_query and sql_fetch_row
	 */
	return RLM_SQL_OK;
}

/*************************************************************************
 *
 *	Function: sql_num_rows
 *
 *	Purpose: database specific num_rows. Returns number of rows in
 *	       query
 *
 *************************************************************************/
static int sql_num_rows(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {

	rlm_sql_freetds_conn_t *conn = handle->conn;
	int	num;

	if (ct_res_info(conn->command, CS_ROW_COUNT, (CS_INT *)&num, CS_UNUSED, NULL) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: error retrieving row count");
		
		return RLM_SQL_ERROR;
	}
	return num;
}


/*************************************************************************
 *
 *	Function: sql_fetch_row
 *
 *	Purpose: database specific fetch_row. Returns a rlm_sql_row_t struct
 *	       with all the data for the query in 'handle->row'. Returns
 *		 0 on success, -1 on failure, RLM_SQL_RECONNECT if 'database is down'.
 *
 *************************************************************************/
static sql_rcode_t sql_fetch_row(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {
	rlm_sql_freetds_conn_t *conn = handle->conn;
	CS_INT ret, count;

	handle->row = NULL;

	ret = ct_fetch(conn->command, CS_UNUSED, CS_UNUSED, CS_UNUSED, &count);
	switch (ret) {
	case CS_FAIL:
		/*
		 *	Serious failure, freetds requires us to cancel the results and maybe even close the db.
		 */
		ERROR("rlm_sql_freetds: failure fetching row data");
		if ((ret = ct_cancel(NULL, conn->command, CS_CANCEL_ALL)) == CS_FAIL) {
			ERROR("rlm_sql_freetds: cleaning up.");
		}
		
		return RLM_SQL_RECONNECT;

	case CS_END_DATA:
		return RLM_SQL_OK;

	case CS_SUCCEED:
		handle->row = conn->results;
		
		return RLM_SQL_OK;

	case CS_ROW_FAIL:
		ERROR("rlm_sql_freetds: recoverable failure fetching row data");
		
		return RLM_SQL_RECONNECT;

	default:
		ERROR("rlm_sql_freetds: unexpected returncode from ct_fetch");
		
		return RLM_SQL_ERROR;
	}
}

/*************************************************************************
 *
 *	Function: sql_free_result
 *
 *	Purpose: database specific free_result. Frees memory allocated
 *	       for a result set
 *
 *************************************************************************/
static sql_rcode_t sql_free_result(UNUSED rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config) {

	/*
	 *	Not implemented, never called from rlm_sql anyway result buffer is freed in the
	 *	finish_query functions.
	 */
	return 0;

}

/*************************************************************************
 *
 *	Function: sql_finish_query
 *
 *	Purpose: End the query, such as freeing memory
 *
 *************************************************************************/
static sql_rcode_t sql_finish_query(rlm_sql_handle_t *handle, UNUSED rlm_sql_config_t *config)
{
	rlm_sql_freetds_conn_t *conn = handle->conn;

	ct_cancel(NULL, conn->command, CS_CANCEL_ALL);

	if (ct_cmd_drop(conn->command) != CS_SUCCEED) {
		ERROR("rlm_sql_freetds: Freeing command structure failed");
		
		return RLM_SQL_ERROR;
	}

	return RLM_SQL_OK;
}

/*************************************************************************
 *
 *	Function: sql_affected_rows
 *
 *	Purpose: Return the number of rows affected by the query (update,
 *	       or insert)
 *
 *************************************************************************/
static int sql_affected_rows(rlm_sql_handle_t *handle, rlm_sql_config_t *config) {
	return sql_num_rows(handle, config);
}

/* Exported to rlm_sql */
rlm_sql_module_t rlm_sql_freetds = {
	"rlm_sql_freetds",
	NULL,
	sql_socket_init,
	sql_query,
	sql_select_query,
	sql_store_result,
	sql_num_fields,
	sql_num_rows,
	sql_fetch_row,
	sql_free_result,
	sql_error,
	sql_finish_query,
	sql_finish_select_query,
	sql_affected_rows
};
