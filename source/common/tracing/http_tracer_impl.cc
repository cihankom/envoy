#include "common/tracing/http_tracer_impl.h"

#include <string>

#include "common/access_log/access_log_formatter.h"
#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/common/macros.h"
#include "common/common/utility.h"
#include "common/http/codes.h"
#include "common/http/header_map_impl.h"
#include "common/http/headers.h"
#include "common/http/utility.h"
#include "common/runtime/uuid_util.h"
#include "common/stream_info/utility.h"

namespace Envoy {
namespace Tracing {

// TODO(mattklein123) PERF: Avoid string creations/copies in this entire file.
static std::string buildResponseCode(const StreamInfo::StreamInfo& info) {
  return info.responseCode() ? std::to_string(info.responseCode().value()) : "0";
}

static std::string valueOrDefault(const Http::HeaderEntry* header, const char* default_value) {
  return header ? header->value().c_str() : default_value;
}

static std::string buildUrl(const Http::HeaderMap& request_headers) {
  std::string path = request_headers.EnvoyOriginalPath()
                         ? request_headers.EnvoyOriginalPath()->value().c_str()
                         : request_headers.Path()->value().c_str();
  static const size_t max_path_length = 128;
  if (path.length() > max_path_length) {
    path = path.substr(0, max_path_length);
  }

  return fmt::format("{}://{}{}", valueOrDefault(request_headers.ForwardedProto(), ""),
                     valueOrDefault(request_headers.Host(), ""), path);
}

const std::string HttpTracerUtility::IngressOperation = "ingress";
const std::string HttpTracerUtility::EgressOperation = "egress";

const std::string& HttpTracerUtility::toString(OperationName operation_name) {
  switch (operation_name) {
  case OperationName::Ingress:
    return IngressOperation;
  case OperationName::Egress:
    return EgressOperation;
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

Decision HttpTracerUtility::isTracing(const StreamInfo::StreamInfo& stream_info,
                                      const Http::HeaderMap& request_headers) {
  // Exclude health check requests immediately.
  if (stream_info.healthCheck()) {
    return {Reason::HealthCheck, false};
  }

  if (!request_headers.RequestId()) {
    return {Reason::NotTraceableRequestId, false};
  }

  // TODO PERF: Avoid copy.
  UuidTraceStatus trace_status =
      UuidUtils::isTraceableUuid(request_headers.RequestId()->value().c_str());

  switch (trace_status) {
  case UuidTraceStatus::Client:
    return {Reason::ClientForced, true};
  case UuidTraceStatus::Forced:
    return {Reason::ServiceForced, true};
  case UuidTraceStatus::Sampled:
    return {Reason::Sampling, true};
  case UuidTraceStatus::NoTrace:
    return {Reason::NotTraceableRequestId, false};
  }

  NOT_REACHED_GCOVR_EXCL_LINE;
}

static void annotateVerbose(Span& span, const StreamInfo::StreamInfo& stream_info) {
  const auto start_time = stream_info.startTime();
  if (stream_info.lastDownstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.lastDownstreamRxByteReceived()),
             Tracing::Logs::get().LastDownstreamRxByteReceived);
  }
  if (stream_info.firstUpstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.firstUpstreamTxByteSent()),
             Tracing::Logs::get().FirstUpstreamTxByteSent);
  }
  if (stream_info.lastUpstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.lastUpstreamTxByteSent()),
             Tracing::Logs::get().LastUpstreamTxByteSent);
  }
  if (stream_info.firstUpstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.firstUpstreamRxByteReceived()),
             Tracing::Logs::get().FirstUpstreamRxByteReceived);
  }
  if (stream_info.lastUpstreamRxByteReceived()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.lastUpstreamRxByteReceived()),
             Tracing::Logs::get().LastUpstreamRxByteReceived);
  }
  if (stream_info.firstDownstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.firstDownstreamTxByteSent()),
             Tracing::Logs::get().FirstDownstreamTxByteSent);
  }
  if (stream_info.lastDownstreamTxByteSent()) {
    span.log(start_time + std::chrono::duration_cast<SystemTime::duration>(
                              *stream_info.lastDownstreamTxByteSent()),
             Tracing::Logs::get().LastDownstreamTxByteSent);
  }
}

void HttpTracerUtility::finalizeSpan(Span& span, const Http::HeaderMap* request_headers,
                                     const StreamInfo::StreamInfo& stream_info,
                                     const Config& tracing_config) {
  // Pre response data.
  if (request_headers) {
    if (request_headers->RequestId()) {
      span.setTag(Tracing::Tags::get().GuidXRequestId,
                  std::string(request_headers->RequestId()->value().c_str()));
    }
    span.setTag(Tracing::Tags::get().HttpUrl, buildUrl(*request_headers));
    span.setTag(Tracing::Tags::get().HttpMethod, request_headers->Method()->value().c_str());
    span.setTag(Tracing::Tags::get().DownstreamCluster,
                valueOrDefault(request_headers->EnvoyDownstreamServiceCluster(), "-"));
    span.setTag(Tracing::Tags::get().UserAgent, valueOrDefault(request_headers->UserAgent(), "-"));
    span.setTag(Tracing::Tags::get().HttpProtocol,
                AccessLog::AccessLogFormatUtils::protocolToString(stream_info.protocol()));

    if (request_headers->ClientTraceId()) {
      span.setTag(Tracing::Tags::get().GuidXClientTraceId,
                  std::string(request_headers->ClientTraceId()->value().c_str()));
    }

    // Build tags based on the custom headers.
    for (const Http::LowerCaseString& header : tracing_config.requestHeadersForTags()) {
      const Http::HeaderEntry* entry = request_headers->get(header);
      if (entry) {
        span.setTag(header.get(), entry->value().c_str());
      }
    }
  }
  span.setTag(Tracing::Tags::get().RequestSize, std::to_string(stream_info.bytesReceived()));

  if (nullptr != stream_info.upstreamHost()) {
    span.setTag(Tracing::Tags::get().UpstreamCluster, stream_info.upstreamHost()->cluster().name());
  }

  // Post response data.
  span.setTag(Tracing::Tags::get().HttpStatusCode, buildResponseCode(stream_info));
  span.setTag(Tracing::Tags::get().ResponseSize, std::to_string(stream_info.bytesSent()));
  span.setTag(Tracing::Tags::get().ResponseFlags,
              StreamInfo::ResponseFlagUtils::toShortString(stream_info));

  if (tracing_config.verbose()) {
    annotateVerbose(span, stream_info);
  }

  if (!stream_info.responseCode() || Http::CodeUtility::is5xx(stream_info.responseCode().value())) {
    span.setTag(Tracing::Tags::get().Error, Tracing::Tags::get().True);
  }

  span.finishSpan();
}

HttpTracerImpl::HttpTracerImpl(DriverPtr&& driver, const LocalInfo::LocalInfo& local_info)
    : driver_(std::move(driver)), local_info_(local_info) {}

SpanPtr HttpTracerImpl::startSpan(const Config& config, Http::HeaderMap& request_headers,
                                  const StreamInfo::StreamInfo& stream_info,
                                  const Tracing::Decision tracing_decision) {
  std::string span_name = HttpTracerUtility::toString(config.operationName());

  if (config.operationName() == OperationName::Egress) {
    span_name.append(" ");
    span_name.append(request_headers.Host()->value().c_str());
  }

  SpanPtr active_span = driver_->startSpan(config, request_headers, span_name,
                                           stream_info.startTime(), tracing_decision);
  if (active_span) {
    active_span->setTag(Tracing::Tags::get().Component, Tracing::Tags::get().Proxy);
    active_span->setTag(Tracing::Tags::get().NodeId, local_info_.nodeName());
    active_span->setTag(Tracing::Tags::get().Zone, local_info_.zoneName());
  }

  return active_span;
}

} // namespace Tracing
} // namespace Envoy
