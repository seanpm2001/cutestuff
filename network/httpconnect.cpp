/*
 * httpconnect.cpp - HTTP "CONNECT" proxy
 * Copyright (C) 2003  Justin Karneges
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include"httpconnect.h"

#include<qstringlist.h>
#include"bsocket.h"
#include"base64.h"

#ifdef PROX_DEBUG
#include<stdio.h>
#endif

#ifdef CS_NAMESPACE
using namespace CS_NAMESPACE;
#endif

static QString extractLine(QByteArray *buf, bool *found)
{
	// scan for newline
	int n;
	for(n = 0; n < (int)buf->size()-1; ++n) {
		if(buf->at(n) == '\r' && buf->at(n+1) == '\n') {
			QCString cstr;
			cstr.resize(n+1);
			memcpy(cstr.data(), buf->data(), n);
			n += 2; // hack off CR/LF

			memmove(buf->data(), buf->data() + n, buf->size() - n);
			buf->resize(buf->size() - n);
			QString s = QString::fromUtf8(cstr);

			if(found)
				*found = true;
			return s;
		}
	}

	if(found)
		*found = false;
	return "";
}

static bool extractMainHeader(const QString &line, QString *proto, int *code, QString *msg)
{
	int n = line.find(' ');
	if(n == -1)
		return false;
	if(proto)
		*proto = line.mid(0, n);
	++n;
	int n2 = line.find(' ', n);
	if(n2 == -1)
		return false;
	if(code)
		*code = line.mid(n, n2-n).toInt();
	n = n2+1;
	if(msg)
		*msg = line.mid(n);
	return true;
}

class HttpConnect::Private
{
public:
	Private() {}

	BSocket sock;
	QString host;
	int port;
	QString user, pass;
	QString real_host;
	int real_port;

	QByteArray recvBuf;

	bool inHeader;
	QStringList headerLines;

	bool active;
};

HttpConnect::HttpConnect(QObject *parent)
:ByteStream(parent)
{
	d = new Private;
	connect(&d->sock, SIGNAL(connected()), SLOT(sock_connected()));
	connect(&d->sock, SIGNAL(connectionClosed()), SLOT(sock_connectionClosed()));
	connect(&d->sock, SIGNAL(delayedCloseFinished()), SLOT(sock_delayedCloseFinished()));
	connect(&d->sock, SIGNAL(readyRead()), SLOT(sock_readyRead()));
	connect(&d->sock, SIGNAL(bytesWritten(int)), SLOT(sock_bytesWritten(int)));
	connect(&d->sock, SIGNAL(error(int)), SLOT(sock_error(int)));

	reset(true);
}

HttpConnect::~HttpConnect()
{
	reset(true);
	delete d;
}

void HttpConnect::reset(bool clear)
{
	if(d->sock.state() != BSocket::Idle)
		d->sock.close();
	if(clear) {
		clearReadBuffer();
		d->recvBuf.resize(0);
	}
	d->active = false;
}

void HttpConnect::setAuth(const QString &user, const QString &pass)
{
	d->user = user;
	d->pass = pass;
}

void HttpConnect::connectToHost(const QString &proxyHost, int proxyPort, const QString &host, int port)
{
	reset(true);

	d->host = proxyHost;
	d->port = proxyPort;
	d->real_host = host;
	d->real_port = port;

#ifdef PROX_DEBUG
	fprintf(stderr, "HttpConnect: Connecting to %s:%d", proxyHost.latin1(), proxyPort);
	if(d->user.isEmpty())
		fprintf(stderr, "\n");
	else
		fprintf(stderr, ", auth {%s,%s}\n", d->user.latin1(), d->pass.latin1());
#endif
	d->sock.connectToHost(d->host, d->port);
}

bool HttpConnect::isOpen() const
{
	return d->active;
}

void HttpConnect::close()
{
	d->sock.close();
	if(d->sock.bytesToWrite() == 0)
		reset();
}

void HttpConnect::write(const QByteArray &buf)
{
	if(d->active)
		d->sock.write(buf);
}

QByteArray HttpConnect::read(int bytes)
{
	if(d->active)
		return ByteStream::read(bytes);
	else
		return QByteArray();
}

int HttpConnect::bytesAvailable() const
{
	if(d->active)
		return ByteStream::bytesAvailable();
	else
		return 0;
}

int HttpConnect::bytesToWrite() const
{
	if(d->active)
		return d->sock.bytesToWrite();
	else
		return 0;
}

void HttpConnect::sock_connected()
{
#ifdef PROX_DEBUG
	fprintf(stderr, "HttpConnect: Connected\n");
#endif
	d->inHeader = true;
	d->headerLines.clear();

	// connected, now send the request
	QString s;
	s += QString("CONNECT ") + d->real_host + ':' + QString::number(d->real_port) + " HTTP/1.0\r\n";
	if(!d->user.isEmpty()) {
		QString str = d->user + ':' + d->pass;
		s += QString("Proxy-Authorization: Basic ") + Base64::encodeString(str) + "\r\n";
	}
	s += "Proxy-Connection: Keep-Alive\r\n";
	s += "Pragma: no-cache\r\n";
	s += "\r\n";

	QCString cs = s.utf8();
	QByteArray block(cs.length());
	memcpy(block.data(), cs.data(), block.size());
	d->sock.write(block);
}

void HttpConnect::sock_connectionClosed()
{
	if(d->active) {
		reset();
		connectionClosed();
	}
	else {
		error(ErrProxyNeg);
	}
}

void HttpConnect::sock_delayedCloseFinished()
{
	if(d->active) {
		reset();
		delayedCloseFinished();
	}
}

void HttpConnect::sock_readyRead()
{
	QByteArray block = d->sock.read();

	if(!d->active) {
		ByteStream::appendArray(&d->recvBuf, block);

		if(d->inHeader) {
			// grab available lines
			while(1) {
				bool found;
				QString line = extractLine(&d->recvBuf, &found);
				if(!found)
					break;
				if(line.isEmpty()) {
					d->inHeader = false;
					break;
				}
				d->headerLines += line;
			}

			// done with grabbing the header?
			if(!d->inHeader) {
				QString str = d->headerLines.first();
				d->headerLines.remove(d->headerLines.begin());

				QString proto;
				int code;
				QString msg;
				if(!extractMainHeader(str, &proto, &code, &msg)) {
#ifdef PROX_DEBUG
					fprintf(stderr, "HttpConnect: invalid header!\n");
#endif
					reset(true);
					error(ErrProxyNeg);
					return;
				}
				else {
#ifdef PROX_DEBUG
					fprintf(stderr, "HttpConnect: header proto=[%s] code=[%d] msg=[%s]\n", proto.latin1(), code, msg.latin1());
					for(QStringList::ConstIterator it = d->headerLines.begin(); it != d->headerLines.end(); ++it)
						fprintf(stderr, "HttpConnect: * [%s]\n", (*it).latin1());
#endif
				}

				if(code == 200) { // OK
#ifdef PROX_DEBUG
					fprintf(stderr, "HttpConnect: << Success >>\n");
#endif
					d->active = true;
					connected();

					if(!d->recvBuf.isEmpty()) {
						appendRead(d->recvBuf);
						d->recvBuf.resize(0);
						readyRead();
					}
				}
				else {
					int err;
					QString errStr;
					if(code == 407) { // Authentication failed
						err = ErrProxyAuth;
						errStr = tr("Authentication failed");
					}
					else if(code == 404) { // Host not found
						err = ErrHostNotFound;
						errStr = tr("Host not found");
					}
					else if(code == 403) { // Access denied
						err = ErrProxyNeg;
						errStr = tr("Access denied");
					}
					else if(code == 503) { // Connection refused
						err = ErrConnectionRefused;
						errStr = tr("Connection refused");
					}
					else { // invalid reply
						err = ErrProxyNeg;
						errStr = tr("Invalid reply");
					}

#ifdef PROX_DEBUG
					fprintf(stderr, "HttpConnect: << Error >> [%s]\n", errStr.latin1());
#endif
					reset(true);
					error(err);
					return;
				}
			}
		}
	}
	else {
		appendRead(block);
		readyRead();
	}
}

void HttpConnect::sock_bytesWritten(int x)
{
	if(d->active)
		bytesWritten(x);
}

void HttpConnect::sock_error(int x)
{
	if(d->active) {
		reset();
		error(ErrRead);
	}
	else {
		reset(true);
		if(x == BSocket::ErrHostNotFound)
			error(ErrProxyConnect);
		else if(x == BSocket::ErrConnectionRefused)
			error(ErrProxyConnect);
		else if(x == BSocket::ErrRead)
			error(ErrProxyNeg);
	}
}
