/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/httpclientconnection.hpp"
#include "base/configtype.hpp"
#include "base/objectlock.hpp"
#include "base/base64.hpp"
#include "base/utility.hpp"
#include "base/logger.hpp"
#include "base/exception.hpp"
#include "base/convert.hpp"
#include "base/tcpsocket.hpp"
#include "base/tlsstream.hpp"
#include "base/networkstream.hpp"

using namespace icinga;

HttpClientConnection::HttpClientConnection(String host, String port, bool tls)
	: m_Host(std::move(host)), m_Port(std::move(port)), m_Tls(tls)
{ }

void HttpClientConnection::Start()
{
	/* Nothing to do here atm. */
}

void HttpClientConnection::Reconnect()
{
	if (m_Stream)
		m_Stream->Close();

	m_Context.~StreamReadContext();
	new (&m_Context) StreamReadContext();

	m_Requests.clear();
	m_CurrentResponse.reset();

	TcpSocket::Ptr socket = new TcpSocket();
	socket->Connect(m_Host, m_Port);

	if (m_Tls)
		m_Stream = new TlsStream(socket, m_Host, RoleClient);
	else
		ASSERT(!"Non-TLS HTTP connections not supported.");
		/* m_Stream = new NetworkStream(socket);
		 * -- does not currently work because the NetworkStream class doesn't support async I/O
		 */

	/* the stream holds an owning reference to this object through the callback we're registering here */
	m_Stream->RegisterDataHandler(std::bind(&HttpClientConnection::DataAvailableHandler, HttpClientConnection::Ptr(this), _1));
	if (m_Stream->IsDataAvailable())
		DataAvailableHandler(m_Stream);
}

Stream::Ptr HttpClientConnection::GetStream() const
{
	return m_Stream;
}

String HttpClientConnection::GetHost() const
{
	return m_Host;
}

String HttpClientConnection::GetPort() const
{
	return m_Port;
}

bool HttpClientConnection::GetTls() const
{
	return m_Tls;
}

void HttpClientConnection::Disconnect()
{
	Log(LogDebug, "HttpClientConnection", "Http client disconnected");

	m_Stream->Shutdown();
}

bool HttpClientConnection::ProcessMessage()
{
	bool res;

	if (m_Requests.empty()) {
		m_Stream->Close();
		return false;
	}

	const std::pair<std::shared_ptr<HttpRequest>, HttpCompletionCallback>& currentRequest = *m_Requests.begin();
	HttpRequest& request = *currentRequest.first.get();
	const HttpCompletionCallback& callback = currentRequest.second;

	if (!m_CurrentResponse)
		m_CurrentResponse = std::make_shared<HttpResponse>(m_Stream, request);

	std::shared_ptr<HttpResponse> currentResponse = m_CurrentResponse;
	HttpResponse& response = *currentResponse.get();

	try {
		res = response.Parse(m_Context, false);
	} catch (const std::exception&) {
		callback(request, response);

		m_Stream->Shutdown();
		return false;
	}

	if (response.Complete) {
		callback(request, response);

		m_Requests.pop_front();
		m_CurrentResponse.reset();

		return true;
	}

	return res;
}

void HttpClientConnection::DataAvailableHandler(const Stream::Ptr& stream)
{
	ASSERT(stream == m_Stream);

	bool close = false;

	if (!m_Stream->IsEof()) {
		boost::mutex::scoped_lock lock(m_DataHandlerMutex);

		try {
			while (ProcessMessage())
				; /* empty loop body */
		} catch (const std::exception& ex) {
			Log(LogWarning, "HttpClientConnection")
				<< "Error while reading Http response: " << DiagnosticInformation(ex);

			close = true;
			Disconnect();
		}
	} else
		close = true;

	if (close)
		m_Stream->Close();
}

std::shared_ptr<HttpRequest> HttpClientConnection::NewRequest()
{
	Reconnect();
	return std::make_shared<HttpRequest>(m_Stream);
}

void HttpClientConnection::SubmitRequest(const std::shared_ptr<HttpRequest>& request,
	const HttpCompletionCallback& callback)
{
	m_Requests.emplace_back(request, callback);
	request->Finish();
}

