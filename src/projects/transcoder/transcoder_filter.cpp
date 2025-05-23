#include "transcoder_filter.h"

#include "filter/filter_resampler.h"
#include "filter/filter_rescaler.h"
#include "transcoder_gpu.h"
#include "transcoder_private.h"

using namespace cmn;

#define PTS_INCREMENT_LIMIT 15

TranscodeFilter::TranscodeFilter()
	: _internal(nullptr)
{
}

TranscodeFilter::~TranscodeFilter()
{
}

std::shared_ptr<TranscodeFilter> TranscodeFilter::Create(int32_t id,
														 const std::shared_ptr<info::Stream>& input_stream_info, std::shared_ptr<MediaTrack> input_track,
														 const std::shared_ptr<info::Stream>& output_stream_info, std::shared_ptr<MediaTrack> output_track,
														 CompleteHandler complete_handler)
{
	auto filter = std::make_shared<TranscodeFilter>();
	if (filter->Configure(id, input_stream_info, input_track, output_stream_info, output_track) == false)
	{
		return nullptr;
	}
	filter->SetCompleteHandler(complete_handler);
	return filter;
}

std::shared_ptr<TranscodeFilter> TranscodeFilter::Create(int32_t id,
														 const std::shared_ptr<info::Stream>& output_stream_info, std::shared_ptr<MediaTrack> output_track,
														 CompleteHandler complete_handler)
{
	auto filter = std::make_shared<TranscodeFilter>();
	if (filter->Configure(id, output_stream_info, output_track, output_stream_info, output_track) == false)
	{
		return nullptr;
	}
	filter->SetCompleteHandler(complete_handler);
	return filter;
}

bool TranscodeFilter::Configure(int32_t id,
								const std::shared_ptr<info::Stream>& input_stream_info, std::shared_ptr<MediaTrack> input_track,
								const std::shared_ptr<info::Stream>& output_stream_info, std::shared_ptr<MediaTrack> output_track)
{
	_id = id;
	_input_stream_info = input_stream_info;
	_input_track = input_track;

	_output_stream_info = output_stream_info;
	_output_track = output_track;
	
	_timestamp_jump_threshold = (int64_t)_input_track->GetTimeBase().GetTimescale() * PTS_INCREMENT_LIMIT;

	return CreateInternal();
}

bool TranscodeFilter::CreateInternal()
{
	std::lock_guard<std::shared_mutex> lock(_mutex);

	// If there is a previously created filter, remove it.
	if (_internal != nullptr)
	{
		_internal->Stop();
		_internal.reset();
		_internal = nullptr;
	}

	switch (_input_track->GetMediaType())
	{
		case MediaType::Audio:
			_internal = std::make_shared<FilterResampler>();
			break;
		case MediaType::Video:
			_internal = std::make_shared<FilterRescaler>();
			break;
		default:
			logte("Unsupported media type in filter");
			return false;
	}

	auto name = ov::String::FormatString("filter_%s", cmn::GetMediaTypeString(_input_track->GetMediaType()));
	auto urn = std::make_shared<info::ManagedQueue::URN>(
		_input_stream_info->GetApplicationName(),
		_input_stream_info->GetName(),
		"trs",
		name.LowerCaseString());
	_internal->SetQueueUrn(urn);
	_internal->SetCompleteHandler(bind(&TranscodeFilter::OnComplete, this, std::placeholders::_1));
	_internal->SetInputTrack(_input_track);
	_internal->SetOutputTrack(_output_track);

	return _internal->Start();
}

void TranscodeFilter::Stop()
{
	std::lock_guard<std::shared_mutex> lock(_mutex);

	if (_internal != nullptr)
	{
		_internal->Stop();
		_internal.reset();
		_internal = nullptr;
	}
}

void TranscodeFilter::Flush()
{
	std::shared_lock<std::shared_mutex> lock(_mutex);
	if (_internal != nullptr)
	{
		// Not implemented
	}
}

bool TranscodeFilter::SendBuffer(std::shared_ptr<MediaFrame> buffer)
{
	if (IsNeedUpdate(buffer) == true)
	{
		if (CreateInternal() == false)
		{
			logte("Failed to regenerate filter");
			return false;
		}

		return true;
	}

	std::shared_lock<std::shared_mutex> lock(_mutex);
	if (_internal == nullptr)
	{
		return false;
	}

	return _internal->SendBuffer(std::move(buffer));
}

bool TranscodeFilter::IsNeedUpdate(std::shared_ptr<MediaFrame> buffer)
{
	// Single track(paired with encoder) does not need to be updated.
	if (_internal == nullptr || _internal->IsSingleTrack() == true)
	{
		return false;
	}

	// In case of pts/dts jumps
	int64_t last_timestamp = _last_timestamp;
	int64_t curr_timestamp = buffer->GetPts();
	_last_timestamp = curr_timestamp;

	// Check #1 - Abnormal timestamp
	int64_t diff_timestamp = abs(curr_timestamp - last_timestamp);
	bool is_abnormal = (last_timestamp != -1LL && diff_timestamp > _timestamp_jump_threshold) ? true : false;
	if (is_abnormal)
	{
		logtw("The timestamp has changed unexpectedly. %lld -> %lld (%lld > %lld)", last_timestamp, buffer->GetPts(), diff_timestamp, _timestamp_jump_threshold);
		return true;
	}

	// Check #2 - Resolution change
	std::shared_lock<std::shared_mutex> lock(_mutex);

	if (_input_track->GetMediaType() == MediaType::Video)
	{
		if (buffer->GetWidth() != (int32_t)_internal->GetInputWidth() ||
			buffer->GetHeight() != (int32_t)_internal->GetInputHeight())
		{
			logti("Changed input resolution of %u track. (%dx%d -> %dx%d)", _input_track->GetId(), _internal->GetInputWidth(), _internal->GetInputHeight(), buffer->GetWidth(), buffer->GetHeight());

			_input_track->SetWidth(buffer->GetWidth());
			_input_track->SetHeight(buffer->GetHeight());

			return true;
		}
	}

	// When using an XMA scaler, resource allocation failures may occur intermittently.
	// Avoid problems in this way until the underlying problem is resolved.
	if (_internal->GetState() == FilterBase::State::ERROR &&
		_input_track->GetCodecModuleId() == cmn::MediaCodecModuleId::XMA &&
		_output_track->GetCodecModuleId() == cmn::MediaCodecModuleId::XMA)
	{
		logtw("It is assumed that the XMA resource allocation failed. So, recreate the filter.");

		return true;
	}

	return false;
}

void TranscodeFilter::SetCompleteHandler(CompleteHandler complete_handler)
{
	_complete_handler = std::move(complete_handler);
}

void TranscodeFilter::OnComplete(std::shared_ptr<MediaFrame> frame)
{
	if (_complete_handler)
	{
		_complete_handler(_id, frame);
	}
}

cmn::Timebase TranscodeFilter::GetInputTimebase() const
{
	return _internal->GetInputTimebase();
}

cmn::Timebase TranscodeFilter::GetOutputTimebase() const
{
	return _internal->GetOutputTimebase();
}

std::shared_ptr<MediaTrack>& TranscodeFilter::GetInputTrack()
{
	return _input_track;
}

std::shared_ptr<MediaTrack>& TranscodeFilter::GetOutputTrack()
{
	return _output_track;
}