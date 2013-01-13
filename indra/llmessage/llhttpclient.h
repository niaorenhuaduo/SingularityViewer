/** 
 * @file llhttpclient.h
 * @brief Declaration of classes for making HTTP client requests.
 *
 * $LicenseInfo:firstyear=2006&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLHTTPCLIENT_H
#define LL_LLHTTPCLIENT_H

/**
 * These classes represent the HTTP client framework.
 */

#include <string>
#include <curl/curl.h>		// CURLcode
#include <boost/intrusive_ptr.hpp>

#include "llassettype.h"
#include "llhttpstatuscodes.h"
#include "aihttpheaders.h"

class LLUUID;
class LLPumpIO;
class LLSD;
class AIHTTPTimeoutPolicy;
class LLBufferArray;
class LLChannelDescriptors;

extern AIHTTPTimeoutPolicy responderIgnore_timeout;
typedef struct _xmlrpc_request* XMLRPC_REQUEST;
typedef struct _xmlrpc_value* XMLRPC_VALUE;

// Output parameter of AICurlPrivate::CurlEasyRequest::getResult.
// Used in XMLRPCResponder.
struct AITransferInfo {
  AITransferInfo() : mSizeDownload(0.0), mTotalTime(0.0), mSpeedDownload(0.0) { }
  F64 mSizeDownload;
  F64 mTotalTime;
  F64 mSpeedDownload;
};

// Events generated by AICurlPrivate::BufferedCurlEasyRequest
struct AIBufferedCurlEasyRequestEvents {
	virtual void received_HTTP_header(void) = 0;										// For example "HTTP/1.0 200 OK", the first header of a reply.
	virtual void received_header(std::string const& key, std::string const& value) = 0;	// Subsequent headers.
	virtual void completed_headers(U32 status, std::string const& reason, AITransferInfo* info) = 0;	// Transaction completed.
};

enum EKeepAlive {
  no_keep_alive = 0,
  keep_alive
};

#ifdef DEBUG_CURLIO
enum EDebugCurl {
  debug_off = 0,
  debug_on
};
#define DEBUG_CURLIO_PARAM(p) ,p
#else
#define DEBUG_CURLIO_PARAM(p)
#endif

class LLHTTPClient {
public:

	/** @name Responder base classes */
	//@{

	/**
	 * @class ResponderBase
	 * @brief Base class for all Responders.
	 *
	 * The life cycle of classes derived from this class is as follows:
	 * They are allocated with new on the line where get(), getByteRange() or post() is called,
	 * and the pointer to the allocated object is then put in a reference counting ResponderPtr.
	 * This ResponderPtr is passed to BufferedCurlEasyRequest::prepRequest which stores it in its
	 * member mResponder. Hence, the life time of a Responder is never longer than its
	 * associated BufferedCurlEasyRequest, however, if everything works correctly, then normally a
	 * responder is deleted in BufferedCurlEasyRequest::processOutput by setting
	 * mReponder to NULL.
	 */
	class ResponderBase : public AIBufferedCurlEasyRequestEvents {
	public:
		typedef boost::shared_ptr<LLBufferArray> buffer_ptr_t;

	protected:
		ResponderBase(void);
		virtual ~ResponderBase();

		// Read body from buffer and put it into content. If status indicates success, interpret it as LLSD, otherwise copy it as-is.
		void decode_llsd_body(U32 status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer, LLSD& content);

		// Read body from buffer and put it into content. Always copy it as-is.
		void decode_raw_body(U32 status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer, std::string& content);

	protected:
		// Associated URL, used for debug output.
		std::string mURL;

		// Headers received from the server.
		AIHTTPReceivedHeaders mReceivedHeaders;

		// The curl result code.
		CURLcode mCode;

		// Set when the transaction finished (with or without errors).
		bool mFinished;

	public:
		// Called to set the URL of the current request for this Responder,
		// used only when printing debug output regarding activity of the Responder.
		void setURL(std::string const& url);

		// Accessors.
		std::string const& getURL(void) const { return mURL; }
		CURLcode result_code(void) const { return mCode; }

		// Called by BufferedCurlEasyRequest::timed_out or BufferedCurlEasyRequest::processOutput.
		virtual void finished(CURLcode code, U32 http_status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer) = 0;

		// Return true if the curl thread is done with this transaction.
		// If this returns true then it is guaranteed that none of the
		// virtual functions will be called anymore: the curl thread
		// will not access this object anymore.
		// Note that normally you don't need to call this function.
		bool is_finished(void) const { return mFinished; }

	protected:
		// AIBufferedCurlEasyRequestEvents
		// These three events are only actually called for classes that implement a needsHeaders() that returns true.

		// Called when the "HTTP/1.x <status> <reason>" header is received.
		/*virtual*/ void received_HTTP_header(void)
		{
			// It's possible that this page was moved (302), so we already saw headers
			// from the 302 page and are starting over on the new page now.
			// Erase all headers EXCEPT the cookies.
			AIHTTPReceivedHeaders set_cookie_headers;
			AIHTTPReceivedHeaders::range_type cookies;
			if (mReceivedHeaders.getValues("set-cookie", cookies))
			{
				for (AIHTTPReceivedHeaders::iterator_type cookie = cookies.first; cookie != cookies.second; ++cookie)
				{
					set_cookie_headers.addHeader(cookie->first, cookie->second);
				}
			}
			// Replace headers with just the cookie headers.
			mReceivedHeaders.swap(set_cookie_headers);
		}

		// Called for all remaining headers.
		/*virtual*/ void received_header(std::string const& key, std::string const& value)
		{
			mReceivedHeaders.addHeader(key, value);
		}

		// Called when the whole transaction is completed (also the body was received), but before the body is processed.
		/*virtual*/ void completed_headers(U32 status, std::string const& reason, AITransferInfo* info)
		{
			completedHeaders(status, reason, mReceivedHeaders);
		}

		// Extract cookie 'key' from mReceivedHeaders and return the string 'key=value', or an empty string if key does not exists.
		std::string const& get_cookie(std::string const& key);

	public:
		// Derived classes that implement completed_headers()/completedHeaders() should return true here.
		virtual bool needsHeaders(void) const { return false; }

		// A derived class should return true if curl should follow redirections.
		// The default is not to follow redirections.
		virtual bool followRedir(void) const { return false; }

		// If this function returns false then we generate an error when a redirect status (300..399) is received.
		virtual bool redirect_status_ok(void) const { return followRedir(); }

		// Timeout policy to use.
		virtual AIHTTPTimeoutPolicy const& getHTTPTimeoutPolicy(void) const = 0;

		// The name of the derived responder object. For debugging purposes.
		virtual char const* getName(void) const = 0;

	protected:
		// Derived classes can override this to get the HTML headers that were received, when the message is completed.
		// Only actually called for classes that implement a needsHeaders() that returns true.
		virtual void completedHeaders(U32 status, std::string const& reason, AIHTTPReceivedHeaders const& headers)
		{
			// The default does nothing.
		}

	private:
		// Used by ResponderPtr. Object is deleted when reference count reaches zero.
		LLAtomicU32 mReferenceCount;

		friend void intrusive_ptr_add_ref(ResponderBase* p);	// Called by boost::intrusive_ptr when a new copy of a boost::intrusive_ptr<ResponderBase> is made.
		friend void intrusive_ptr_release(ResponderBase* p);	// Called by boost::intrusive_ptr when a boost::intrusive_ptr<ResponderBase> is destroyed.
																// This function must delete the ResponderBase object when the reference count reaches zero.
	};

	// Responders derived from this base class should use HTTPClient::head or HTTPClient::getHeaderOnly.
	// That will set the curl option CURLOPT_NOBODY so that only headers are received.
	class ResponderHeadersOnly : public ResponderBase {
	private:
		/*virtual*/ bool needsHeaders(void) const { return true; }
		/*virtual*/ bool followRedir(void) const { return true; }

	protected:
		// ResponderBase event

		// The responder finished. Do not override this function in derived classes; override completedRaw instead.
		/*virtual*/ void finished(CURLcode code, U32 http_status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer)
		{
			mCode = code;
			// Allow classes derived from ResponderHeadersOnly to override completedHeaders.
			completedHeaders(http_status, reason, mReceivedHeaders);
			mFinished = true;
		}

	protected:
#ifdef SHOW_ASSERT
		// Responders derived from this class must override completedHeaders.
		// They may not attempt to override any of the virual functions defined by ResponderBase.
		// Define those functions here with different parameters in order to cause a compile
		// warning when a class accidently tries to override them.
		enum YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS { };
		virtual void completedRaw(YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS) { }
		virtual void completed(YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS) { }
		virtual void result(YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS) { }
		virtual void errorWithContent(YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS) { }
		virtual void error(YOU_MAY_ONLY_OVERRIDE_COMPLETED_HEADERS) { }
#endif
	};

	/**
	 * @class ResponderWithCompleted
	 * @brief Base class for Responders that implement completed, or completedRaw if the response is not LLSD.
	 */
	class ResponderWithCompleted : public ResponderBase {
	protected:
		// ResponderBase event

		// The responder finished. Do not override this function in derived classes; override completedRaw instead.
		/*virtual*/ void finished(CURLcode code, U32 http_status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer)
		{
			mCode = code;
			// Allow classes derived from ResponderWithCompleted to override completedRaw
			// (if not they should override completed or be derived from ResponderWithResult instead).
			completedRaw(http_status, reason, channels, buffer);
			mFinished = true;
		}

	protected:
		// Events generated by this class.

		// Derived classes can override this to get the raw data of the body of the HTML message that was received.
		// The default is to interpret the content as LLSD and call completed().
		virtual void completedRaw(U32 status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer);

		// ... or, derived classes can override this to get LLSD content when the message is completed.
		// The default aborts, as it should never be called (can't make it pure virtual though, so
		// classes that override completedRaw don't need to implement this function, too).
		virtual void completed(U32 status, std::string const& reason, LLSD const& content);

#ifdef SHOW_ASSERT
		// Responders derived from this class must override either completedRaw or completed.
		// They may not attempt to override any of the virual functions defined by ResponderBase.
		// Define those functions here with different parameters in order to cause a compile
		// warning when a class accidently tries to override them.
		enum YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS { };
		virtual void result(YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS) { }
		virtual void errorWithContent(YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS) { }
		virtual void error(YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS) { }
#endif
	};

	/**
	 * @class ResponderWithResult
	 * @brief Base class for reponders that expect LLSD in the body of the reply.
	 *
	 * Classes derived from ResponderWithResult must implement result, and either errorWithContent or error.
	 */
	class ResponderWithResult : public ResponderBase {
	protected:
		// The responder finished. Do not override this function in derived classes; use ResponderWithCompleted instead.
		/*virtual*/ void finished(CURLcode code, U32 http_status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer);

	protected:
		// Events generated by this class.

		// Derived classes must override this to receive the content of a body upon success.
		virtual void result(LLSD const& content) = 0;

		// Derived classes can override this to get informed when a bad HTML status code is received.
		// The default calls error().
		virtual void errorWithContent(U32 status, std::string const& reason, LLSD const& content);

		// ... or, derived classes can override this to get informed when a bad HTML status code is received.
		// The default prints the error to llinfos.
		virtual void error(U32 status, std::string const& reason);

	public:
		// Called from LLSDMessage::ResponderAdapter::listener.
		// LLSDMessage::ResponderAdapter is a hack, showing among others by fact that it needs these functions.

		void pubErrorWithContent(CURLcode code, U32 status, std::string const& reason, LLSD const& content) { mCode = code; errorWithContent(status, reason, content); mFinished = true; }
		void pubResult(LLSD const& content) { mCode = CURLE_OK; result(content); mFinished = true; }

#ifdef SHOW_ASSERT
		// Responders derived from this class must override result, and either errorWithContent or error.
		// They may not attempt to override any of the virual functions defined by ResponderWithCompleted.
		// Define those functions here with different parameter in order to cause a compile
		// warning when a class accidently tries to override them.
		enum YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS { };
		virtual void completedRaw(YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS) { }
		virtual void completed(YOU_ARE_DERIVING_FROM_THE_WRONG_CLASS) { }
#endif
	};

	/**
	 * @class LegacyPolledResponder
	 * @brief As ResponderWithCompleted but caches the result for polling.
	 *
	 * This class allows old polling code to poll if the transaction finished
	 * by calling is_finished() (from the main the thread) and then access the
	 * results-- as opposed to immediately digesting the results when any of
	 * the virtual functions are called.
	 */
	class LegacyPolledResponder : public ResponderWithCompleted {
	protected:
		U32 mStatus;
		std::string mReason;

	protected:
		// The responder finished. Do not override this function in derived classes.
		/*virtual*/ void finished(CURLcode code, U32 http_status, std::string const& reason, LLChannelDescriptors const& channels, buffer_ptr_t const& buffer)
		{
			mStatus = http_status;
			mReason = reason;
			// Call base class implementation.
			ResponderWithCompleted::finished(code, http_status, reason, channels, buffer);
		}

	public:
		LegacyPolledResponder(void) : mStatus(HTTP_INTERNAL_ERROR) { }

		// Accessors.
		U32 http_status(void) const { return mStatus; }
		std::string const& reason(void) const { return mReason; }
	};

	/**
	 * @class ResponderIgnoreBody
	 * @brief Base class for responders that ignore the result body.
	 */
	class ResponderIgnoreBody : public ResponderWithResult {
		void result(LLSD const&) { }
	};

	/**
	 * @class ResponderIgnore
	 * @brief Responder that ignores the reply, if any, from the server.
	 */
	class ResponderIgnore : public ResponderIgnoreBody {
		/*virtual*/ AIHTTPTimeoutPolicy const& getHTTPTimeoutPolicy(void) const { return responderIgnore_timeout;}
		/*virtual*/ char const* getName(void) const { return "ResponderIgnore"; }
	};

	// A Responder is passed around as ResponderPtr, which causes it to automatically
	// destruct when there are no pointers left pointing to it.
	typedef boost::intrusive_ptr<ResponderBase> ResponderPtr;

	//@}

	/** @name non-blocking API */
	//@{
	static void head(std::string const& url, ResponderHeadersOnly* responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void head(std::string const& url, ResponderHeadersOnly* responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; head(url, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void getByteRange(std::string const& url, S32 offset, S32 bytes, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void getByteRange(std::string const& url, S32 offset, S32 bytes, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; getByteRange(url, offset, bytes, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void get(std::string const& url, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void get(std::string const& url, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; get(url, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void get(std::string const& url, LLSD const& query, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void get(std::string const& url, LLSD const& query, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; get(url, query, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void put(std::string const& url, LLSD const& body, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void put(std::string const& url, LLSD const& body, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; put(url, body, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void getHeaderOnly(std::string const& url, ResponderHeadersOnly* responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void getHeaderOnly(std::string const& url, ResponderHeadersOnly* responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; getHeaderOnly(url, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	static void post(std::string const& url, LLSD const& body, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void post(std::string const& url, LLSD const& body, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; post(url, body, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	/** Takes ownership of request and deletes it when sent */
	static void postXMLRPC(std::string const& url, XMLRPC_REQUEST request, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void postXMLRPC(std::string const& url, XMLRPC_REQUEST request, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; postXMLRPC(url, request, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	static void postXMLRPC(std::string const& url, char const* method, XMLRPC_VALUE value, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void postXMLRPC(std::string const& url, char const* method, XMLRPC_VALUE value, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; postXMLRPC(url, method, value, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	/** Takes ownership of data and deletes it when sent */
	static void postRaw(std::string const& url, const char* data, S32 size, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void postRaw(std::string const& url, const char* data, S32 size, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; postRaw(url, data, size, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	static void postFile(std::string const& url, std::string const& filename, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void postFile(std::string const& url, std::string const& filename, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; postFile(url, filename, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	static void postFile(std::string const& url, const LLUUID& uuid, LLAssetType::EType asset_type, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive);
	static void postFile(std::string const& url, const LLUUID& uuid, LLAssetType::EType asset_type, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off), EKeepAlive keepalive = keep_alive)
	    { AIHTTPHeaders headers; postFile(url, uuid, asset_type, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug), keepalive); }

	static void del(std::string const& url, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void del(std::string const& url, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; del(url, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

		///< sends a DELETE method, but we can't call it delete in c++
	
	/**
	 * @brief Send a MOVE webdav method
	 *
	 * @param url The complete serialized (and escaped) url to get.
	 * @param destination The complete serialized destination url.
	 * @param responder The responder that will handle the result.
	 * @param headers A map of key:value headers to pass to the request
	 * @param timeout The number of seconds to give the server to respond.
	 */
	static void move(std::string const& url, std::string const& destination, ResponderPtr responder, AIHTTPHeaders& headers/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
	static void move(std::string const& url, std::string const& destination, ResponderPtr responder/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off))
	    { AIHTTPHeaders headers; move(url, destination, responder, headers/*,*/ DEBUG_CURLIO_PARAM(debug)); }

	//@}

	/**
	 * @brief Blocking HTTP GET that returns an LLSD map of status and body.
	 *
	 * @param url the complete serialized (and escaped) url to get
	 * @return An LLSD of { 'status':status, 'body':payload }
	 */
	static LLSD blockingGet(std::string const& url/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));

	/**
	 * @brief Blocking HTTP GET that returns the raw body.
	 *
	 * @param url the complete serialized (and escaped) url to get
	 * @param result the target string to write the body to
	 * @return HTTP status
	 */
	static U32 blockingGetRaw(const std::string& url, std::string& result/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));

	/**
	 * @brief Blocking HTTP POST that returns an LLSD map of status and body.
	 *
	 * @param url the complete serialized (and escaped) url to get
	 * @param body the LLSD post body
	 * @return An LLSD of { 'status':status (an int), 'body':payload (an LLSD) }
	 */
	static LLSD blockingPost(std::string const& url, LLSD const& body/*,*/ DEBUG_CURLIO_PARAM(EDebugCurl debug = debug_off));
};

#endif // LL_LLHTTPCLIENT_H
