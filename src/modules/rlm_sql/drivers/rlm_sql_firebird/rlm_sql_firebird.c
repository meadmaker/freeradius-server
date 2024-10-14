/*
 * sql_firebird.c Part of Firebird rlm_sql driver
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
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * @copyright 2006 The FreeRADIUS server project
 * @copyright 2006 Vitaly Bodzhgua (vitaly@eastera.net)
 */
RCSID("$Id$")

#define LOG_PREFIX "sql - firebird"

#include "sql_fbapi.h"
#include <freeradius-devel/util/debug.h>

static int _sql_socket_destructor(rlm_sql_firebird_conn_t *conn)
{
	DEBUG2("socket destructor called, closing socket");

	fb_commit(conn);
	if (conn->dbh) {
		fb_free_statement(conn);
		isc_detach_database(conn->status, &(conn->dbh));

		if (fb_error(conn)) {
			WARN("Got error "
			       "when closing socket: %s", conn->error);
		}
	}

	talloc_free_children(conn);

	return 0;
}

/** Establish connection to the db
 *
 */
static sql_rcode_t sql_socket_init(rlm_sql_handle_t *handle, rlm_sql_config_t const *config,
				   UNUSED fr_time_delta_t timeout)
{
	rlm_sql_firebird_conn_t	*conn;

	long res;

	MEM(conn = handle->conn = talloc_zero(handle, rlm_sql_firebird_conn_t));
	talloc_set_destructor(conn, _sql_socket_destructor);

	res = fb_init_socket(conn);
	if (res) return RLM_SQL_ERROR;

	if (fb_connect(conn, config)) {
		ERROR("Connection failed: %s", conn->error);

		return RLM_SQL_RECONNECT;
	}

	return 0;
}

/** Issue a query to the database.
 *
 */
static unlang_action_t sql_query(rlm_rcode_t *p_result, UNUSED int *priority, UNUSED request_t *request, void *uctx)
{
	fr_sql_query_t		*query_ctx = talloc_get_type_abort(uctx, fr_sql_query_t);
	rlm_sql_firebird_conn_t *conn = query_ctx->handle->conn;

	int deadlock = 0;

	try_again:
	/*
	 *	Try again query when deadlock, because in any case it
	 *	will be retried.
	 */
	if (fb_sql_query(conn, query_ctx->query_str)) {
		/* but may be lost for short sessions */
		if ((conn->sql_code == DEADLOCK_SQL_CODE) &&
		    !deadlock) {
			DEBUG("conn_id deadlock. Retry query %s", query_ctx->query_str);

			/*
			 *	@todo For non READ_COMMITED transactions put
			 *	rollback here
			 *	fb_rollback(conn);
			 */
			deadlock = 1;
			goto try_again;
		}

		ERROR("conn_id rlm_sql_firebird,sql_query error: sql_code=%li, error='%s', query=%s",
		      (long int) conn->sql_code, conn->error, query_ctx->query_str);

		if (conn->sql_code == DOWN_SQL_CODE) {
			query_ctx->rcode = RLM_SQL_RECONNECT;
			RETURN_MODULE_FAIL;
		}

		/* Free problem query */
		if (fb_rollback(conn)) {
			//assume the network is down if rollback had failed
			ERROR("Fail to rollback transaction after previous error: %s", conn->error);

			query_ctx->rcode = RLM_SQL_RECONNECT;
			RETURN_MODULE_FAIL;
		}
		//   conn->in_use=0;

		query_ctx->rcode = RLM_SQL_ERROR;
		RETURN_MODULE_FAIL;
	}

	if (conn->statement_type != isc_info_sql_stmt_select) {
		if (fb_commit(conn)) {
			query_ctx->rcode = RLM_SQL_ERROR;
			RETURN_MODULE_FAIL;
		}
	}

	query_ctx->rcode = RLM_SQL_OK;
	RETURN_MODULE_OK;
}

/** Returns name of fields.
 *
 */
static sql_rcode_t sql_fields(char const **out[], fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	rlm_sql_firebird_conn_t	*conn = query_ctx->handle->conn;

	int		fields, i;
	char const	**names;

	fields = conn->sqlda_out->sqld;
	if (fields <= 0) return RLM_SQL_ERROR;

	MEM(names = talloc_array(query_ctx, char const *, fields));

	for (i = 0; i < fields; i++) names[i] = conn->sqlda_out->sqlvar[i].sqlname;
	*out = names;

	return RLM_SQL_OK;
}

/** Returns an individual row.
 *
 */
static unlang_action_t sql_fetch_row(rlm_rcode_t *p_result, UNUSED int *priority, UNUSED request_t *request, void *uctx)
{
	fr_sql_query_t		*query_ctx = talloc_get_type_abort(uctx, fr_sql_query_t);
	rlm_sql_handle_t	*handle = query_ctx->handle;
	rlm_sql_firebird_conn_t *conn = handle->conn;
	int res;

	query_ctx->row = NULL;

	if (conn->statement_type != isc_info_sql_stmt_exec_procedure) {
		res = fb_fetch(conn);
		if (res == 100) {
			query_ctx->rcode = RLM_SQL_NO_MORE_ROWS;
			RETURN_MODULE_OK;
		}

		if (res) {
			ERROR("Fetch problem: %s", conn->error);

			query_ctx->rcode = RLM_SQL_ERROR;
			RETURN_MODULE_FAIL;
		}
	} else {
		conn->statement_type = 0;
	}

	fb_store_row(conn);

	query_ctx->row = conn->row;

	query_ctx->rcode = RLM_SQL_OK;
	RETURN_MODULE_OK;
}

/** End the select query, such as freeing memory or result.
 *
 */
static sql_rcode_t sql_finish_select_query(fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	rlm_sql_firebird_conn_t *conn = (rlm_sql_firebird_conn_t *) query_ctx->handle->conn;

	fb_commit(conn);
	fb_close_cursor(conn);

	return 0;
}

/** End the query
 *
 */
static sql_rcode_t sql_finish_query(UNUSED fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	return 0;
}

/** Frees memory allocated for a result set.
 *
 */
static sql_rcode_t sql_free_result(UNUSED fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	return 0;
}

/** Retrieves any errors associated with the query context
 *
 * @note Caller will free any memory allocated in ctx.
 *
 * @param ctx to allocate temporary error buffers in.
 * @param out Array of sql_log_entrys to fill.
 * @param outlen Length of out array.
 * @param query_ctx Query context to retrieve error for.
 * @param config rlm_sql config.
 * @return number of errors written to the #sql_log_entry_t array.
 */
static size_t sql_error(UNUSED TALLOC_CTX *ctx, sql_log_entry_t out[], NDEBUG_UNUSED size_t outlen,
			fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	rlm_sql_firebird_conn_t *conn = query_ctx->handle->conn;

	fr_assert(conn);
	fr_assert(outlen > 0);

	if (!conn->error) return 0;

	out[0].type = L_ERR;
	out[0].msg = conn->error;

	return 1;
}

/** Return the number of rows affected by the query (update, or insert)
 *
 */
static int sql_affected_rows(fr_sql_query_t *query_ctx, UNUSED rlm_sql_config_t const *config)
{
	return fb_affected_rows(query_ctx->handle->conn);
}

/* Exported to rlm_sql */
extern rlm_sql_driver_t rlm_sql_firebird;
rlm_sql_driver_t rlm_sql_firebird = {
	.common = {
		.name				= "sql_firebird",
		.magic				= MODULE_MAGIC_INIT
	},
	.sql_socket_init		= sql_socket_init,
	.sql_query			= sql_query,
	.sql_select_query		= sql_query,
	.sql_num_rows			= sql_affected_rows,
	.sql_affected_rows		= sql_affected_rows,
	.sql_fetch_row			= sql_fetch_row,
	.sql_fields			= sql_fields,
	.sql_free_result		= sql_free_result,
	.sql_error			= sql_error,
	.sql_finish_query		= sql_finish_query,
	.sql_finish_select_query	= sql_finish_select_query
};
