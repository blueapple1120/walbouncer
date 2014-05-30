#include "wbmasterconn.h"

#include<errno.h>
#include<poll.h>
#include<string.h>

#include "xfutils.h"
#include "xf_pg_config.h"

static bool libpq_select(PGconn *mc, int timeout_ms);
static void process_walsender_message(ReplMessage *msg);
static void xlf_send(PGconn *mc, const char *buffer, int nbytes);
static void xlf_send_reply(PGconn *mc, bool force, bool requestReply);

// TODO: move these to a structure
static char *recvBuf = NULL;
static XLogRecPtr latestWalEnd = 0;
static TimestampTz latestSendTime = 0;

PGconn* xlf_open_connection(const char *conninfo)
{
	PGconn* masterConn = PQconnectdb(conninfo);
	if (PQstatus(masterConn) != CONNECTION_OK)
		error(PQerrorMessage(masterConn));

	return masterConn;
}


bool xlf_startstreaming(PGconn *mc, XLogRecPtr pos, TimeLineID tli)
{
	char cmd[256];
	PGresult *res;

	xf_info("Start streaming from master at %X/%X", FormatRecPtr(pos));

	snprintf(cmd, sizeof(cmd),
			"START_REPLICATION %X/%X TIMELINE %u",
			(uint32) (pos>>32), (uint32) pos, tli);
	res = PQexec(mc, cmd);

	if (PQresultStatus(res) == PGRES_COMMAND_OK)
	{
		PQclear(res);
		return false;
	}
	else if (PQresultStatus(res) != PGRES_COPY_BOTH)
	{
		PQclear(res);
		error(PQerrorMessage(mc));
	}
	PQclear(res);
	return true;
}

void
xlf_endstreaming(PGconn *mc, TimeLineID *next_tli)
{
	PGresult   *res;
	int i = 0;

	if (PQputCopyEnd(mc, NULL) <= 0 || PQflush(mc))
		error(PQerrorMessage(mc));

	/*
	 * After COPY is finished, we should receive a result set indicating the
	 * next timeline's ID, or just CommandComplete if the server was shut
	 * down.
	 *
	 * If we had not yet received CopyDone from the backend, PGRES_COPY_IN
	 * would also be possible. However, at the moment this function is only
	 * called after receiving CopyDone from the backend - the walreceiver
	 * never terminates replication on its own initiative.
	 */
	res = PQgetResult(mc);
	while (PQresultStatus(res) == PGRES_COPY_OUT)
	{
		int copyresult;
		do {
			char *buf;
			copyresult = PQgetCopyData(mc, &buf, 0);
			if (buf)
				PQfreemem(buf);

		} while (copyresult >= 0);
		res = PQgetResult(mc);
	}

	if (PQresultStatus(res) == PGRES_TUPLES_OK)
	{
		/*
		 * Read the next timeline's ID. The server also sends the timeline's
		 * starting point, but it is ignored.
		 */
		if (PQnfields(res) < 2 || PQntuples(res) != 1)
			error("unexpected result set after end-of-streaming");
		*next_tli = ensure_atoi(PQgetvalue(res, 0, 0));
		PQclear(res);

		/* the result set should be followed by CommandComplete */
		res = PQgetResult(mc);
	}
	else
		*next_tli = 0;

	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		error(PQerrorMessage(mc));

	/* Verify that there are no more results */
	res = PQgetResult(mc);
	while (res!=NULL) {
		xf_info("Status: %d", PQresultStatus(res));
		res = PQgetResult(mc);
	}

	if (res != NULL)
		error("unexpected result after CommandComplete: %s", PQerrorMessage(mc));

}

/*
 * Wait until we can read WAL stream, or timeout.
 *
 * Returns true if data has become available for reading, false if timed out
 * or interrupted by signal.
 *
 * This is based on pqSocketCheck.
 */
static bool
libpq_select(PGconn *mc, int timeout_ms)
{
	int			ret;

	Assert(mc != NULL);
	if (PQsocket(mc) < 0)
		error("socket not open");

	/* We use poll(2) if available, otherwise select(2) */
	{
		struct pollfd input_fd;

		input_fd.fd = PQsocket(mc);
		input_fd.events = POLLIN | POLLERR;
		input_fd.revents = 0;

		ret = poll(&input_fd, 1, timeout_ms);
	}

	if (ret == 0 || (ret < 0 && errno == EINTR))
		return false;
	if (ret < 0)
		error("select() failed: %m");
	return true;
}

int
xlf_receive(PGconn *mc, int timeout, char **buffer)
{
	int			rawlen;

	if (recvBuf != NULL)
		PQfreemem(recvBuf);
	recvBuf = NULL;

	/* Try to receive a CopyData message */
	rawlen = PQgetCopyData(mc, &recvBuf, 1);
	if (rawlen == 0)
	{
		/*
		 * No data available yet. If the caller requested to block, wait for
		 * more data to arrive.
		 */
		if (timeout > 0)
		{
			if (!libpq_select(mc, timeout))
				return 0;
		}

		if (PQconsumeInput(mc) == 0)
			showPQerror(mc, "could not receive data from WAL stream");

		/* Now that we've consumed some input, try again */
		rawlen = PQgetCopyData(mc, &recvBuf, 1);
		if (rawlen == 0)
			return 0;
	}
	if (rawlen == -1)			/* end-of-streaming or error */
	{
		PGresult   *res;

		res = PQgetResult(mc);
		if (PQresultStatus(res) == PGRES_COMMAND_OK ||
			PQresultStatus(res) == PGRES_COPY_IN)
		{
			PQclear(res);
			return -1;
		}
		else
		{
			PQclear(res);
			showPQerror(mc, "could not receive data from WAL stream");
		}
	}
	if (rawlen < -1)
		showPQerror(mc, "could not receive data from WAL stream");

	/* Return received messages to caller */
	*buffer = recvBuf;
	return rawlen;
}

static void
process_walsender_message(ReplMessage *msg)
{
	latestWalEnd = msg->walEnd;
	latestSendTime = msg->sendTime;
}

/*
 * Send a message to XLOG stream.
 *
 * ereports on error.
 */
static void
xlf_send(PGconn *mc, const char *buffer, int nbytes)
{
	if (PQputCopyData(mc, buffer, nbytes) <= 0 ||
		PQflush(mc))
		showPQerror(mc, "could not send data to WAL stream");
}

static void
xlf_send_reply(PGconn *mc, bool force, bool requestReply)
{
	XLogRecPtr writePtr = latestWalEnd;
	XLogRecPtr flushPtr = latestWalEnd;
	XLogRecPtr	applyPtr = latestWalEnd;
	TimestampTz sendTime = latestSendTime;
	TimestampTz now;
	char reply_message[1+4*8+1+1];

	/*
	 * If the user doesn't want status to be reported to the master, be sure
	 * to exit before doing anything at all.

	if (!force && wal_receiver_status_interval <= 0)
		return;*/

	/* Get current timestamp. */
	now = latestSendTime;//GetCurrentTimestamp();

	/*
	 * We can compare the write and flush positions to the last message we
	 * sent without taking any lock, but the apply position requires a spin
	 * lock, so we don't check that unless something else has changed or 10
	 * seconds have passed.  This means that the apply log position will
	 * appear, from the master's point of view, to lag slightly, but since
	 * this is only for reporting purposes and only on idle systems, that's
	 * probably OK.
	 */
	/*if (!force
		&& writePtr == LogstreamResult.Write
		&& flushPtr == LogstreamResult.Flush
		&& !TimestampDifferenceExceeds(sendTime, now,
									   wal_receiver_status_interval * 1000))
		return;*/
	sendTime = now;

	/* Construct a new message */

	//resetStringInfo(&reply_message);
	memset(reply_message, 0, sizeof(reply_message));
	//pq_sendbyte(&reply_message, 'r');
	reply_message[0] = 'r';
	//pq_sendint64(&reply_message, writePtr);
	write64(&(reply_message[1]), writePtr);
	//pq_sendint64(&reply_message, flushPtr);
	write64(&(reply_message[9]), flushPtr);
	//pq_sendint64(&reply_message, applyPtr);
	write64(&(reply_message[17]), applyPtr);
	//pq_sendint64(&reply_message, GetCurrentIntegerTimestamp());
	write64(&(reply_message[25]), sendTime);
	//pq_sendbyte(&reply_message, requestReply ? 1 : 0);
	reply_message[33] = requestReply ? 1 : 0;

	/* Send it */
	/*elog(DEBUG2, "sending write %X/%X flush %X/%X apply %X/%X%s",
		 (uint32) (writePtr >> 32), (uint32) writePtr,
		 (uint32) (flushPtr >> 32), (uint32) flushPtr,
		 (uint32) (applyPtr >> 32), (uint32) applyPtr,
		 requestReply ? " (reply requested)" : "");*/

	xf_info("Send reply: %lu %lu %lu %d\n", writePtr, flushPtr, applyPtr, requestReply);
	xlf_send(mc, reply_message, 34);
}

void
xlf_process_message(PGconn *mc, char *buf, size_t len,
		ReplMessage *msg)
{
	switch (buf[0])
	{
		case 'w':
			{
				msg->type = MSG_WAL_DATA;
				msg->dataStart = fromnetwork64(buf+1);
				msg->walEnd = fromnetwork64(buf+9);
				msg->sendTime = fromnetwork64(buf+17);
				msg->replyRequested = 0;

				msg->dataPtr = 0;
				msg->dataLen = len - 25;
				msg->data = buf+25;
				msg->nextPageBoundary = (XLOG_BLCKSZ - msg->dataStart) & (XLOG_BLCKSZ-1);

				xf_info("Received %lu byte WAL block\n", len-25);
				xf_info("   dataStart: %X/%X\n", FormatRecPtr(msg->dataStart));
				xf_info("   walEnd: %lu\n", msg->walEnd);
				xf_info("   sendTime: %lu\n", msg->sendTime);

				process_walsender_message(msg);
				break;
			}
		case 'k':
			{
				xf_info("Keepalive message\n");
				msg->type = MSG_KEEPALIVE;
				msg->walEnd = fromnetwork64(buf+1);
				msg->sendTime = fromnetwork64(buf+9);
				msg->replyRequested = *(buf+17);

				xf_info("   walEnd: %lu\n", msg->walEnd);
				xf_info("   sendTime: %lu\n", msg->sendTime);
				xf_info("   replyRequested: %d\n", msg->replyRequested);
				process_walsender_message(msg);

				if (msg->replyRequested)
					xlf_send_reply(mc, true, false);
				break;
			}
	}
}

bool
xlf_identify_system(PGconn* mc,
		char** primary_sysid, char** primary_tli, char** primary_xpos)
{
	PGresult *result = PQexec(mc, "IDENTIFY_SYSTEM");
	if (PQresultStatus(result) != PGRES_TUPLES_OK)
	{
		PQclear(result);
		error(PQerrorMessage(mc));
	}
	if (PQnfields(result) < 3 || PQntuples(result) != 1)
	{
		error("Invalid response");
	}

	if (primary_sysid)
		*primary_sysid = xfstrdup(PQgetvalue(result, 0, 0));
	if (primary_tli)
		*primary_tli = xfstrdup(PQgetvalue(result, 0, 1));
	if (primary_xpos)
		*primary_xpos = xfstrdup(PQgetvalue(result, 0, 2));

	PQclear(result);
	return true;
}

Oid *
xlf_find_tablespace_oids(const char *conninfo, const char* tablespace_names)
{
	PGconn* masterConn = xlf_open_connection(conninfo);
	Oid *oids;
	PGresult *res;
	int oidcount;
	int i;

	const char *paramValues[1] = {tablespace_names};
	res = PQexecParams(masterConn,
		"SELECT oid FROM pg_tablespace WHERE spcname = "
		"ANY (string_to_array($1, ',')) "
		"OR spcname IN ('pg_default', 'pg_global')",
		1, NULL, paramValues,
		NULL, NULL, 0);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		error("Could not retrieve tablespaces: %s", PQerrorMessage(masterConn));

	oidcount = PQntuples(res);
	oids = xfalloc0(sizeof(Oid)*(oidcount+1));

	for (i = 0; i < oidcount; i++)
	{
		char *oid = PQgetvalue(res, i, 0);
		xf_info("Found tablespace oid %s %d", oid, atoi(oid));
		oids[i] = atoi(oid);
	}
	PQclear(res);

	PQfinish(masterConn);

	return oids;
}
