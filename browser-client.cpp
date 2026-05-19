/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "browser-client.hpp"
#include "obs-browser-source.hpp"
#include "base64/base64.hpp"
#include <nlohmann/json.hpp>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <QApplication>
#include <QThread>
#include <QToolTip>
#if defined(__APPLE__) && CHROME_VERSION_BUILD > 4430
#include <IOSurface/IOSurface.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>

#include "drm-format.hpp"
#endif

inline bool BrowserClient::valid() const
{
	return !!bs && !bs->destroying;
}

CefRefPtr<CefLoadHandler> BrowserClient::GetLoadHandler()
{
	return this;
}

CefRefPtr<CefRenderHandler> BrowserClient::GetRenderHandler()
{
	return this;
}

CefRefPtr<CefDisplayHandler> BrowserClient::GetDisplayHandler()
{
	return this;
}

CefRefPtr<CefLifeSpanHandler> BrowserClient::GetLifeSpanHandler()
{
	return this;
}

CefRefPtr<CefContextMenuHandler> BrowserClient::GetContextMenuHandler()
{
	return this;
}

CefRefPtr<CefAudioHandler> BrowserClient::GetAudioHandler()
{
	return reroute_audio ? this : nullptr;
}

CefRefPtr<CefRequestHandler> BrowserClient::GetRequestHandler()
{
	return this;
}

CefRefPtr<CefResourceRequestHandler> BrowserClient::GetResourceRequestHandler(CefRefPtr<CefBrowser>,
									      CefRefPtr<CefFrame>,
									      CefRefPtr<CefRequest> request, bool, bool,
									      const CefString &, bool &)
{
	if (request->GetHeaderByName("origin") == "null") {
		return this;
	}

	return nullptr;
}

void BrowserClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser>, TerminationStatus
#if CHROME_VERSION_BUILD >= 6367
					      ,
					      int, const CefString &error_string
#endif
)
{
	if (!valid())
		return;

#if CHROME_VERSION_BUILD >= 6367
	std::string str_text = error_string;
#else
	std::string str_text = "<unknown>";
#endif

	const char *sourceName = "<unknown>";

	if (bs && bs->source)
		sourceName = obs_source_get_name(bs->source);

	blog(LOG_ERROR, "[obs-browser: '%s'] Webpage has crashed unexpectedly! Reason: '%s'", sourceName,
	     str_text.c_str());
}

CefResourceRequestHandler::ReturnValue BrowserClient::OnBeforeResourceLoad(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
									   CefRefPtr<CefRequest>,
									   CefRefPtr<CefCallback>)
{
	return RV_CONTINUE;
}

bool BrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
#if CHROME_VERSION_BUILD >= 6834
				  int,
#endif
				  const CefString &, const CefString &, cef_window_open_disposition_t, bool,
				  const CefPopupFeatures &, CefWindowInfo &, CefRefPtr<CefClient> &,
				  CefBrowserSettings &, CefRefPtr<CefDictionaryValue> &, bool *)
{
	/* block popups */
	return true;
}

void BrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
					CefRefPtr<CefMenuModel> model)
{
	/* remove all context menu contributions */
	model->Clear();
}

bool BrowserClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, CefProcessId,
					     CefRefPtr<CefProcessMessage> message)
{
	const std::string &name = message->GetName();
	CefRefPtr<CefListValue> input_args = message->GetArgumentList();
	nlohmann::json json;

	if (!valid()) {
		return false;
	}

	// Fall-through switch, so that higher levels also have lower-level rights
	switch (webpage_control_level) {
	case ControlLevel::All:
		if (name == "startRecording") {
			obs_frontend_recording_start();
		} else if (name == "stopRecording") {
			obs_frontend_recording_stop();
		} else if (name == "startStreaming") {
			obs_frontend_streaming_start();
		} else if (name == "stopStreaming") {
			obs_frontend_streaming_stop();
		} else if (name == "pauseRecording") {
			obs_frontend_recording_pause(true);
		} else if (name == "unpauseRecording") {
			obs_frontend_recording_pause(false);
		} else if (name == "startVirtualcam") {
			obs_frontend_start_virtualcam();
		} else if (name == "stopVirtualcam") {
			obs_frontend_stop_virtualcam();
		}
		[[fallthrough]];
	case ControlLevel::Advanced:
		if (name == "startReplayBuffer") {
			obs_frontend_replay_buffer_start();
		} else if (name == "stopReplayBuffer") {
			obs_frontend_replay_buffer_stop();
		} else if (name == "setCurrentScene") {
			const std::string scene_name = input_args->GetString(1).ToString();
			OBSSourceAutoRelease source = obs_get_source_by_name(scene_name.c_str());
			if (!source) {
				blog(LOG_WARNING,
				     "Browser source '%s' tried to switch to scene '%s' which doesn't exist",
				     obs_source_get_name(bs->source), scene_name.c_str());
			} else if (!obs_source_is_scene(source)) {
				blog(LOG_WARNING, "Browser source '%s' tried to switch to '%s' which isn't a scene",
				     obs_source_get_name(bs->source), scene_name.c_str());
			} else {
				obs_frontend_set_current_scene(source);
			}
		} else if (name == "setCurrentTransition") {
			const std::string transition_name = input_args->GetString(1).ToString();
			obs_frontend_source_list transitions = {};
			obs_frontend_get_transitions(&transitions);

			OBSSourceAutoRelease transition;
			for (size_t i = 0; i < transitions.sources.num; i++) {
				obs_source_t *source = transitions.sources.array[i];
				if (obs_source_get_name(source) == transition_name) {
					transition = obs_source_get_ref(source);
					break;
				}
			}

			obs_frontend_source_list_free(&transitions);

			if (transition)
				obs_frontend_set_current_transition(transition);
			else
				blog(LOG_WARNING,
				     "Browser source '%s' tried to change the current transition to '%s' which doesn't exist",
				     obs_source_get_name(bs->source), transition_name.c_str());
		}
		[[fallthrough]];
	case ControlLevel::Basic:
		if (name == "saveReplayBuffer") {
			obs_frontend_replay_buffer_save();
		}
		[[fallthrough]];
	case ControlLevel::ReadUser:
		if (name == "getScenes") {
			struct obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			std::vector<nlohmann::json> scenes_vector;
			for (size_t i = 0; i < list.sources.num; i++) {
				obs_source_t *source = list.sources.array[i];
				scenes_vector.push_back(obs_source_get_name(source));
			}
			json = scenes_vector;
			obs_frontend_source_list_free(&list);
		} else if (name == "getCurrentScene") {
			OBSSourceAutoRelease current_scene = obs_frontend_get_current_scene();

			if (!current_scene)
				return false;

			const char *name = obs_source_get_name(current_scene);
			if (!name)
				return false;

			json = {{"name", name},
				{"width", obs_source_get_width(current_scene)},
				{"height", obs_source_get_height(current_scene)}};
		} else if (name == "getTransitions") {
			struct obs_frontend_source_list list = {};
			obs_frontend_get_transitions(&list);
			std::vector<nlohmann::json> transitions_vector;
			for (size_t i = 0; i < list.sources.num; i++) {
				obs_source_t *source = list.sources.array[i];
				transitions_vector.push_back(obs_source_get_name(source));
			}
			json = transitions_vector;
			obs_frontend_source_list_free(&list);
		} else if (name == "getCurrentTransition") {
			OBSSourceAutoRelease source = obs_frontend_get_current_transition();
			json = obs_source_get_name(source);
		}
		[[fallthrough]];
	case ControlLevel::ReadObs:
		if (name == "getStatus") {
			json = {{"recording", obs_frontend_recording_active()},
				{"streaming", obs_frontend_streaming_active()},
				{"recordingPaused", obs_frontend_recording_paused()},
				{"replaybuffer", obs_frontend_replay_buffer_active()},
				{"virtualcam", obs_frontend_virtualcam_active()}};
		}
		[[fallthrough]];
	case ControlLevel::None:
		if (name == "getControlLevel") {
			json = (int)webpage_control_level;
		}
	}

	CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("executeCallback");

	CefRefPtr<CefListValue> execute_args = msg->GetArgumentList();
	execute_args->SetInt(0, input_args->GetInt(0));
	execute_args->SetString(1, json.dump());

	SendBrowserProcessMessage(browser, PID_RENDERER, msg);

	return true;
}

void BrowserClient::GetViewRect(CefRefPtr<CefBrowser>, CefRect &rect)
{
	if (!valid()) {
		rect.Set(0, 0, 16, 16);
		return;
	}

	rect.Set(0, 0, bs->width < 1 ? 1 : bs->width, bs->height < 1 ? 1 : bs->height);
}

bool BrowserClient::OnTooltip(CefRefPtr<CefBrowser>, CefString &text)
{
	std::string str_text = text;
	QMetaObject::invokeMethod(QCoreApplication::instance()->thread(),
				  [str_text]() { QToolTip::showText(QCursor::pos(), str_text.c_str()); });
	return true;
}

void BrowserClient::OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &, const void *buffer,
			    int width, int height)
{
	if (type != PET_VIEW) {
		// TODO Overlay texture on top of bs->texture
		return;
	}

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
	if (sharing_available) {
		return;
	}
#endif

	if (!valid()) {
		return;
	}

	if (bs->width != width || bs->height != height) {
		obs_enter_graphics();
		bs->DestroyTextures();
		obs_leave_graphics();
	}

	if (!bs->texture && width && height) {
		obs_enter_graphics();
		bs->texture = gs_texture_create(width, height, GS_BGRA, 1, (const uint8_t **)&buffer, GS_DYNAMIC);
		bs->width = width;
		bs->height = height;
		obs_leave_graphics();
	} else {
		obs_enter_graphics();
		gs_texture_set_image(bs->texture, (const uint8_t *)buffer, width * 4, false);
		obs_leave_graphics();
	}
}

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
void BrowserClient::UpdateExtraTexture()
{
	if (bs->texture) {
		const uint32_t cx = gs_texture_get_width(bs->texture);
		const uint32_t cy = gs_texture_get_height(bs->texture);
		const gs_color_format format = gs_texture_get_color_format(bs->texture);
		const gs_color_format linear_format = gs_generalize_format(format);

		if (linear_format != format) {
			if (!bs->extra_texture || bs->last_format != linear_format || bs->last_cx != cx ||
			    bs->last_cy != cy) {
				if (bs->extra_texture) {
					gs_texture_destroy(bs->extra_texture);
					bs->extra_texture = nullptr;
				}
				bs->extra_texture = gs_texture_create(cx, cy, linear_format, 1, nullptr, 0);
				bs->last_cx = cx;
				bs->last_cy = cy;
				bs->last_format = linear_format;
			}
		} else if (bs->extra_texture) {
			gs_texture_destroy(bs->extra_texture);
			bs->extra_texture = nullptr;
			bs->last_cx = 0;
			bs->last_cy = 0;
			bs->last_format = GS_UNKNOWN;
		}
	}
}

void BrowserClient::OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &,
#if CHROME_VERSION_BUILD >= 6367
				       const CefAcceleratedPaintInfo &info)
#else
				       void *shared_handle)
#endif
{
	if (type != PET_VIEW) {
		// TODO Overlay texture on top of bs->texture
		return;
	}

	if (!valid()) {
		return;
	}

#if !defined(_WIN32) && !defined(__APPLE__)
	if (info.plane_count == 0)
		return;

	struct obs_cef_video_format format = obs_cef_format_from_cef_type(info.format);
	uint64_t modifier = info.modifier;

	if (format.gs_format == GS_UNKNOWN)
		return;

	uint32_t *strides = (uint32_t *)alloca(info.plane_count * sizeof(uint32_t));
	uint32_t *offsets = (uint32_t *)alloca(info.plane_count * sizeof(uint32_t));
	uint64_t *modifiers = (uint64_t *)alloca(info.plane_count * sizeof(uint64_t));
	int *fds = (int *)alloca(info.plane_count * sizeof(int));

	/* NOTE: This a workaround under X11 where the modifier is always invalid where it can mean "no modifier" in
	 * Chromium's code. */
	if (obs_get_nix_platform() == OBS_NIX_PLATFORM_X11_EGL && modifier == DRM_FORMAT_MOD_INVALID)
		modifier = DRM_FORMAT_MOD_LINEAR;

	for (size_t i = 0; i < kAcceleratedPaintMaxPlanes; i++) {
		auto *plane = &info.planes[i];

		strides[i] = plane->stride;
		offsets[i] = plane->offset;
		fds[i] = plane->fd;

		modifiers[i] = modifier;
	}
#endif

#if !defined(_WIN32) && CHROME_VERSION_BUILD < 6367
	if (shared_handle == bs->last_handle)
		return;
#endif

	obs_enter_graphics();

	if (bs->texture) {
#ifdef _WIN32
		//gs_texture_release_sync(bs->texture, 0);
#endif
		gs_texture_destroy(bs->texture);
		bs->texture = nullptr;
	}

#if defined(__APPLE__) && CHROME_VERSION_BUILD > 6367
	bs->texture = gs_texture_create_from_iosurface((IOSurfaceRef)(uintptr_t)info.shared_texture_io_surface);
#elif defined(__APPLE__) && CHROME_VERSION_BUILD > 4183
	bs->texture = gs_texture_create_from_iosurface((IOSurfaceRef)(uintptr_t)shared_handle);
#elif defined(_WIN32) && CHROME_VERSION_BUILD > 4183
	bs->texture =
#if CHROME_VERSION_BUILD >= 6367
		gs_texture_open_nt_shared((uint32_t)(uintptr_t)info.shared_texture_handle);
#else
		gs_texture_open_nt_shared((uint32_t)(uintptr_t)shared_handle);
#endif
	//if (bs->texture)
	//	gs_texture_acquire_sync(bs->texture, 1, INFINITE);

#elif defined(_WIN32)
	bs->texture = gs_texture_open_shared((uint32_t)(uintptr_t)shared_handle);
#else
	bs->texture = gs_texture_create_from_dmabuf(info.extra.coded_size.width, info.extra.coded_size.height,
						    format.drm_format, format.gs_format, info.plane_count, fds, strides,
						    offsets, modifier != DRM_FORMAT_MOD_INVALID ? modifiers : NULL);
#endif
	UpdateExtraTexture();
	obs_leave_graphics();

#if defined(__APPLE__) && CHROME_VERSION_BUILD >= 6367
	bs->last_handle = info.shared_texture_io_surface;
#elif defined(_WIN32) && CHROME_VERSION_BUILD >= 6367
	bs->last_handle = info.shared_texture_handle;
#elif defined(__APPLE__) || defined(_WIN32)
	bs->last_handle = shared_handle;
#endif
}

#ifdef CEF_ON_ACCELERATED_PAINT2
void BrowserClient::OnAcceleratedPaint2(CefRefPtr<CefBrowser>, PaintElementType type, const RectList &,
					void *shared_handle, bool new_texture)
{
	if (type != PET_VIEW) {
		// TODO Overlay texture on top of bs->texture
		return;
	}

	if (!valid()) {
		return;
	}

	if (!new_texture) {
		return;
	}

	obs_enter_graphics();

	if (bs->texture) {
		gs_texture_destroy(bs->texture);
		bs->texture = nullptr;
	}

#if defined(__APPLE__) && CHROME_VERSION_BUILD > 4183
	bs->texture = gs_texture_create_from_iosurface((IOSurfaceRef)(uintptr_t)shared_handle);
#elif defined(_WIN32) && CHROME_VERSION_BUILD > 4183
	bs->texture = gs_texture_open_nt_shared((uint32_t)(uintptr_t)shared_handle);

#else
	bs->texture = gs_texture_open_shared((uint32_t)(uintptr_t)shared_handle);
#endif
	UpdateExtraTexture();
	obs_leave_graphics();
}
#endif
#endif

static speaker_layout GetSpeakerLayout(CefAudioHandler::ChannelLayout cefLayout)
{
	switch (cefLayout) {
	case CEF_CHANNEL_LAYOUT_MONO:
		return SPEAKERS_MONO; /**< Channels: MONO */
	case CEF_CHANNEL_LAYOUT_STEREO:
		return SPEAKERS_STEREO; /**< Channels: FL, FR */
	case CEF_CHANNEL_LAYOUT_2POINT1:
	case CEF_CHANNEL_LAYOUT_2_1:
	case CEF_CHANNEL_LAYOUT_SURROUND:
		return SPEAKERS_2POINT1; /**< Channels: FL, FR, LFE */
	case CEF_CHANNEL_LAYOUT_2_2:
	case CEF_CHANNEL_LAYOUT_QUAD:
	case CEF_CHANNEL_LAYOUT_4_0:
		return SPEAKERS_4POINT0; /**< Channels: FL, FR, FC, RC */
	case CEF_CHANNEL_LAYOUT_4_1:
	case CEF_CHANNEL_LAYOUT_5_0:
	case CEF_CHANNEL_LAYOUT_5_0_BACK:
		return SPEAKERS_4POINT1; /**< Channels: FL, FR, FC, LFE, RC */
	case CEF_CHANNEL_LAYOUT_5_1:
	case CEF_CHANNEL_LAYOUT_5_1_BACK:
		return SPEAKERS_5POINT1; /**< Channels: FL, FR, FC, LFE, RL, RR */
	case CEF_CHANNEL_LAYOUT_7_1:
	case CEF_CHANNEL_LAYOUT_7_1_WIDE_BACK:
	case CEF_CHANNEL_LAYOUT_7_1_WIDE:
		return SPEAKERS_7POINT1; /**< Channels: FL, FR, FC, LFE, RL, RR, SL, SR */
	default:
		return SPEAKERS_UNKNOWN;
	}
}

void BrowserClient::OnAudioStreamStarted(CefRefPtr<CefBrowser> browser, const CefAudioParameters &params_,
					 int channels_)
{
	UNUSED_PARAMETER(browser);
	channels = channels_;
	channel_layout = (ChannelLayout)params_.channel_layout;
	sample_rate = params_.sample_rate;
	frames_per_buffer = params_.frames_per_buffer;
}

void BrowserClient::OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float **data, int frames, int64_t pts)
{
	UNUSED_PARAMETER(browser);
	if (!valid()) {
		return;
	}
	struct obs_source_audio audio = {};
	const uint8_t **pcm = (const uint8_t **)data;
	speaker_layout speakers = GetSpeakerLayout(channel_layout);
	int speaker_count = get_audio_channels(speakers);
	for (int i = 0; i < speaker_count; i++)
		audio.data[i] = pcm[i];
	audio.samples_per_sec = sample_rate;
	audio.frames = frames;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.speakers = speakers;
	audio.timestamp = (uint64_t)pts * 1000000LLU;
	obs_source_output_audio(bs->source, &audio);
}

void BrowserClient::OnAudioStreamStopped(CefRefPtr<CefBrowser> browser)
{
	UNUSED_PARAMETER(browser);
}

void BrowserClient::OnAudioStreamError(CefRefPtr<CefBrowser> browser, const CefString &message)
{
	UNUSED_PARAMETER(browser);
	UNUSED_PARAMETER(message);
}

static CefAudioHandler::ChannelLayout Convert2CEFSpeakerLayout(int channels)
{
	switch (channels) {
	case 1:
		return CEF_CHANNEL_LAYOUT_MONO;
	case 2:
		return CEF_CHANNEL_LAYOUT_STEREO;
	case 3:
		return CEF_CHANNEL_LAYOUT_2_1;
	case 4:
		return CEF_CHANNEL_LAYOUT_4_0;
	case 5:
		return CEF_CHANNEL_LAYOUT_4_1;
	case 6:
		return CEF_CHANNEL_LAYOUT_5_1;
	case 8:
		return CEF_CHANNEL_LAYOUT_7_1;
	default:
		return CEF_CHANNEL_LAYOUT_UNSUPPORTED;
	}
}

bool BrowserClient::GetAudioParameters(CefRefPtr<CefBrowser> browser, CefAudioParameters &params)
{
	UNUSED_PARAMETER(browser);
	int channels = (int)audio_output_get_channels(obs_get_audio());
	params.channel_layout = Convert2CEFSpeakerLayout(channels);
	params.sample_rate = (int)audio_output_get_sample_rate(obs_get_audio());
	params.frames_per_buffer = kFramesPerBuffer;
	return true;
}

void BrowserClient::OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int)
{
	if (!valid()) {
		return;
	}

	if (frame->IsMain() && bs->autofill_username.length() && bs->autofill_password.length()) {
		std::string user = CefURIEncode(bs->autofill_username, false).ToString();
		std::string pass = CefURIEncode(bs->autofill_password, false).ToString();
		std::string uuid = CefURIEncode(bs->autofill_device_uuid, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  var u = decodeURIComponent('" + user + "');";
		script += "  var p = decodeURIComponent('" + pass + "');";
		script += "  var d = decodeURIComponent('" + uuid + "');";
		script += "  if (d) localStorage.setItem('deviceUuid', d);";
		script += "  if (d) {";
		script += "    var _origFetch = window.fetch;";
		script += "    window.fetch = function(url, opts) {";
		script += "      if (url && typeof url === 'string' && url.indexOf('inplayip.tv') !== -1) {";
		script += "        opts = opts ? Object.assign({}, opts) : {};";
		script += "        var hdr = new Headers(opts.headers || {});";
		script += "        if (!hdr.has('deviceuuid')) hdr.set('deviceuuid', d);";
		script += "        opts.headers = hdr;";
		script += "        if (opts.body && typeof opts.body === 'string') {";
		script += "          try {";
		script += "            var body = JSON.parse(opts.body);";
		script += "            if (!body.deviceUuid) { body.deviceUuid = d; opts.body = JSON.stringify(body); }";
		script += "          } catch(e) {}";
		script += "        }";
		script += "      }";
		script += "      return _origFetch.apply(this, arguments);";
		script += "    };";
		script += "    window.dispatchEvent(new StorageEvent('storage', {key:'deviceUuid', newValue:d}));";
		script += "  }";
		script += "  var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;";
		script += "  function fill() {";
		script += "    var email = document.querySelector('input[type=\"email\"].login');";
		script += "    var pwd = document.querySelector('input[type=\"password\"].login');";
		script += "    var btn = document.querySelector('button.login-btn');";
		script += "    if (!email || !pwd || !btn) { setTimeout(fill, 300); return; }";
		script += "    setter.call(email, u);";
		script += "    email.dispatchEvent(new Event('input', {bubbles:true}));";
		script += "    email.dispatchEvent(new Event('change', {bubbles:true}));";
		script += "    setter.call(pwd, p);";
		script += "    pwd.dispatchEvent(new Event('input', {bubbles:true}));";
		script += "    pwd.dispatchEvent(new Event('change', {bubbles:true}));";
		script += "    var tries = 0;";
		script += "    var wait = setInterval(function() {";
		script += "      tries++;";
		script += "      if (!btn.disabled) { clearInterval(wait); btn.click(); }";
		script += "      else if (tries > 30) { clearInterval(wait); btn.removeAttribute('disabled'); btn.click(); }";
		script += "    }, 100);";
		script += "  }";
		script += "  fill();";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->autofill_event_ids_enabled && bs->autofill_event_ids.length()) {
		std::string encoded = CefURIEncode(bs->autofill_event_ids, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsInplayScheduler) return;";
		script += "  window.__obsInplayScheduler = true;";
		script += "  (function patchSLDP() {";
		script += "    if (window.SLDP && window.SLDP.init) {";
		script += "      var _orig = window.SLDP.init;";
		script += "      window.SLDP.init = function(opts) { opts.muted = false; return _orig.call(this, opts); };";
		script += "    } else {";
		script += "      setTimeout(patchSLDP, 500);";
		script += "    }";
		script += "  })();";
		script += "  var raw = decodeURIComponent('" + encoded + "');";
		script += "  var eventIds = raw.split(/[\\n\\r,]+/).map(function(s) { return parseInt(s.trim(), 10); }).filter(function(n) { return !isNaN(n) && n > 0; });";
		script += "  if (!eventIds.length) return;";
		script += "  var handledIds = {};";
		script += "  function getRkey() {";
		script += "    var btns = document.querySelectorAll('.play-btn');";
		script += "    if (!btns.length) return null;";
		script += "    return Object.keys(btns[0]).find(function(k) { return k.startsWith('__reactInternalInstance') || k.startsWith('__reactFiber'); });";
		script += "  }";
		script += "  function findBtn(targetId) {";
		script += "    var btns = document.querySelectorAll('.play-btn');";
		script += "    var rkey = getRkey();";
		script += "    if (!rkey || !btns.length) return null;";
		script += "    for (var i = 0; i < btns.length; i++) {";
		script += "      var node = btns[i][rkey];";
		script += "      while (node) {";
		script += "        if (node.memoizedProps && node.memoizedProps.item) {";
		script += "          var id = node.memoizedProps.item.eventId || node.memoizedProps.item.wtScheduledEventId;";
		script += "          if (id == targetId) return btns[i];";
		script += "        }";
		script += "        node = node.return;";
		script += "      }";
		script += "    }";
		script += "    return null;";
		script += "  }";
		script += "  function doPlay(targetId, retries) {";
		script += "    var found = findBtn(targetId);";
		script += "    if (!found) {";
		script += "      if ((retries || 0) < 15) { setTimeout(function() { doPlay(targetId, (retries || 0) + 1); }, 2000); }";
		script += "      else { console.warn('[obs-scheduler] btn nao encontrado para id', targetId); delete handledIds[targetId]; }";
		script += "      return;";
		script += "    }";
		script += "    found.click();";
		script += "    setTimeout(function() {";
		script += "      var sdBtn = document.querySelector('#scheduleDropdown .quality-section button[title=\"SD Live\"]');";
		script += "      if (sdBtn) sdBtn.click();";
		script += "      setTimeout(function() {";
		script += "        var arrowUp = document.querySelector('.fa-arrow-up');";
		script += "        if (arrowUp) (arrowUp.closest('button, a') || arrowUp.parentElement).click();";
		script += "        setTimeout(function() {";
		script += "          document.querySelectorAll('video').forEach(function(v) { v.muted = false; v.volume = 1; });";
		script += "        }, 1000);";
		script += "      }, 300);";
		script += "    }, 300);";
		script += "  }";
		script += "  function resetLiveFilter(cb) {";
		script += "    var liveChk = document.getElementById('LIVE');";
		script += "    var newChk = document.getElementById('NEW');";
		script += "    var vodChk = document.getElementById('VOD');";
		script += "    if (liveChk && liveChk.checked && (!newChk || !newChk.checked) && (!vodChk || !vodChk.checked)) {";
		script += "      var btns = document.querySelectorAll('.filters__buttons_actions button');";
		script += "      for (var i = 0; i < btns.length; i++) {";
		script += "        if (btns[i].textContent.trim() === 'Reset') { btns[i].click(); setTimeout(cb, 800); return; }";
		script += "      }";
		script += "    }";
		script += "    cb();";
		script += "  }";
		script += "  function playByEventId(targetId) {";
		script += "    var arrowDown = document.querySelector('.fa-arrow-down');";
		script += "    if (arrowDown) {";
		script += "      (arrowDown.closest('button, a') || arrowDown.parentElement).click();";
		script += "      setTimeout(function() { resetLiveFilter(function() { doPlay(targetId, 0); }); }, 2500);";
		script += "    } else {";
		script += "      resetLiveFilter(function() { doPlay(targetId, 0); });";
		script += "    }";
		script += "  }";
		script += "  function scheduleAll(data) {";
		script += "    var arr = Array.isArray(data) ? data : [];";
		script += "    var now = Date.now();";
		script += "    var bestLive = null;";
		script += "    eventIds.forEach(function(id) {";
		script += "      if (handledIds[id]) return;";
		script += "      var ev = arr.find(function(e) { return e.eventId === id || e.wtScheduledEventId === id; });";
		script += "      if (!ev) return;";
		script += "      var startMs = new Date(ev.startTime + 'Z').getTime() - 600000;";
		script += "      var delay = startMs - now;";
		script += "      if (delay <= 0) {";
		script += "        handledIds[id] = true;";
		script += "        if (ev.wssLink && (!bestLive || startMs > bestLive.startMs)) {";
		script += "          bestLive = { id: id, startMs: startMs };";
		script += "        }";
		script += "      } else {";
		script += "        handledIds[id] = true;";
		script += "        setTimeout(function() { playByEventId(id); }, delay);";
		script += "      }";
		script += "    });";
		script += "    if (bestLive) { playByEventId(bestLive.id); }";
		script += "  }";
		script += "  function localDate() { var d = new Date(); return d.getFullYear() + '-' + String(d.getMonth()+1).padStart(2,'0') + '-' + String(d.getDate()).padStart(2,'0'); }";
		script += "  function init() {";
		script += "    if (window.__obsSchedulerStop) return;";
		script += "    var token = localStorage.getItem('token');";
		script += "    if (!token) { setTimeout(init, 2000); return; }";
		script += "    var uuid = localStorage.getItem('deviceUuid');";
		script += "    fetch('https://api.inplayip.tv/api/schedule/table', {";
		script += "      method: 'POST',";
		script += "      headers: { 'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json', 'deviceuuid': uuid },";
		script += "      body: JSON.stringify({ filters: { searchDate: localDate(), showVOD: false }, timezoneOffset: new Date().getTimezoneOffset() })";
		script += "    })";
		script += "    .then(function(r) { return r.json(); })";
		script += "    .then(function(data) { scheduleAll(data); if (!window.__obsSchedulerStop) setTimeout(init, 120000); })";
		script += "    .catch(function() { if (!window.__obsSchedulerStop) setTimeout(init, 5000); });";
		script += "  }";
		script += "  window.__obsSchedulerRestart = init;";
		script += "  init();";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->betfair_username.length() && bs->betfair_password.length()) {
		std::string user = CefURIEncode(bs->betfair_username, false).ToString();
		std::string pass = CefURIEncode(bs->betfair_password, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  var hostname = window.location.hostname;";
		script += "  if (hostname.indexOf('betfair') === -1) return;";
		script += "  if (hostname.indexOf('livevideo.betfair') !== -1) {";
		script += "    if (window.__obsBfKeepAlive) return;";
		script += "    window.__obsBfKeepAlive = true;";
		script += "    setInterval(function() {";
		script += "      var img = new Image();";
		script += "      img.src = 'https://www.betfair.bet.br/favicon.ico?' + Date.now();";
		script += "    }, 20 * 60 * 1000);";
		script += "    setInterval(function() {";
		script += "      var loginEl = document.querySelector('#ssc-liu, input[name=\"username\"], .ssc-s-form');";
		script += "      if (loginEl && loginEl.offsetParent !== null) {";
		script += "        console.error('[obs-betfair] sessao expirada, relogando...');";
		script += "        setTimeout(function() {";
		script += "          window.location.href = 'https://www.betfair.bet.br/exchange/plus/?obs_relogin=1';";
		script += "        }, 2000);";
		script += "      }";
		script += "    }, 15000);";
		script += "    return;";
		script += "  }";
		script += "  if (document.referrer.indexOf('livevideo.betfair') !== -1 || window.location.search.indexOf('obs_relogin') !== -1) {";
		script += "    sessionStorage.removeItem('_bf_obs_redirected');";
		script += "  }";
		script += "  if (sessionStorage.getItem('_bf_obs_redirected')) return;";
		script += "  var u = decodeURIComponent('" + user + "');";
		script += "  var p = decodeURIComponent('" + pass + "');";
		script += "  var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;";
		script += "  function fillInput(el, val) {";
		script += "    el.focus();";
		script += "    el.dispatchEvent(new Event('focus', {bubbles:true}));";
		script += "    setter.call(el, val);";
		script += "    el.dispatchEvent(new Event('input', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('change', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('keyup', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('blur', {bubbles:true}));";
		script += "  }";
		script += "  function goLivevideo() {";
		script += "    sessionStorage.setItem('_bf_obs_redirected', '1');";
		script += "    window.location.href = 'https://livevideo.betfair.bet.br/';";
		script += "  }";
		// After submit: poll DOM until #ssc-liu disappears or becomes hidden.
		// Covers both AJAX login (no page reload) and full-page-redirect login.
		script += "  function watchForLoginSuccess() {";
		script += "    var done = false;";
		script += "    var checks = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      if (done) { clearInterval(iv); return; }";
		script += "      checks++;";
		script += "      var el = document.querySelector('#ssc-liu');";
		script += "      if (!el || el.offsetParent === null) {";
		script += "        done = true; clearInterval(iv); goLivevideo();";
		script += "      } else if (checks > 30) { clearInterval(iv); }";
		script += "    }, 500);";
		script += "  }";
		script += "  function tryFillForm() {";
		script += "    var userInput = document.querySelector('#ssc-liu') ||";
		script += "      document.querySelector('input[name=\"username\"]') ||";
		script += "      document.querySelector('input[placeholder*=\"usu\" i]') ||";
		script += "      document.querySelector('input[placeholder*=\"email\" i]');";
		script += "    var passInput = document.querySelector('#ssc-lipw') ||";
		script += "      document.querySelector('input[type=\"password\"]');";
		script += "    if (!userInput || !passInput || userInput.offsetParent === null) return false;";
		script += "    fillInput(userInput, u);";
		script += "    setTimeout(function() {";
		script += "      fillInput(passInput, p);";
		script += "      setTimeout(function() {";
		script += "        var submitBtn = document.querySelector('#ssc-lis') ||";
		script += "          document.querySelector('button[type=\"submit\"]') ||";
		script += "          document.querySelector('input[type=\"submit\"]') ||";
		script += "          document.querySelector('button[class*=\"ssc\"]') ||";
		script += "          document.querySelector('.ssc-s-form button');";
		script += "        if (!submitBtn) return;";
		script += "        var tries = 0;";
		script += "        var wait = setInterval(function() {";
		script += "          tries++;";
		script += "          if (!submitBtn.disabled) {";
		script += "            clearInterval(wait); submitBtn.click(); watchForLoginSuccess();";
		script += "          } else if (tries > 30) {";
		script += "            clearInterval(wait); submitBtn.removeAttribute('disabled'); submitBtn.click(); watchForLoginSuccess();";
		script += "          }";
		script += "        }, 100);";
		script += "      }, 400);";
		script += "    }, 300);";
		script += "    return true;";
		script += "  }";
		script += "  var attempts = 0;";
		script += "  function poll() {";
		script += "    if (tryFillForm()) return;";
		script += "    attempts++;";
		script += "    if (attempts < 8) { setTimeout(poll, 1000); }";
		script += "    else { goLivevideo(); }";
		script += "  }";
		script += "  setTimeout(poll, 1000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->betfair_market_ids.length()) {
		std::string encoded = CefURIEncode(bs->betfair_market_ids, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsBetfairScheduler) return;";
		script += "  window.__obsBetfairScheduler = true;";
		script += "  if (window.location.hostname.indexOf('livevideo.betfair') === -1) return;";
		script += "  console.error('[obs-tz] offset=' + new Date().getTimezoneOffset() + ' localDate=' + new Date().getDate() + '/' + (new Date().getMonth()+1) + ' localHour=' + new Date().getHours() + ' isoDate=' + new Date().toISOString().substring(0,10));";
		script += "  var raw = decodeURIComponent('" + encoded + "');";
		script += "  var marketIds = raw.split(/[\\n\\r,]+/).map(function(s) { return s.trim(); }).filter(function(s) { return s.length > 0; });";
		script += "  if (!marketIds.length) return;";
		script += "  var MONTHS = {Jan:0,Feb:1,Mar:2,Apr:3,May:4,Jun:5,Jul:6,Aug:7,Sep:8,Oct:9,Nov:10,Dec:11};";
		script += "  var handled = {};";
		script += "  function parseTime(txt) {";
		script += "    var m = txt.match(/(\\d+):(\\d+)\\s+\\w+\\s+(\\d+)\\s+(\\w+)/);";
		script += "    if (!m) return null;";
		script += "    var d = new Date();";
		script += "    d.setSeconds(0, 0);";
		script += "    d.setHours(parseInt(m[1]), parseInt(m[2]));";
		script += "    var mon = MONTHS[m[4]];";
		script += "    if (mon !== undefined) d.setMonth(mon, parseInt(m[3]));";
		script += "    return d;";
		script += "  }";
		script += "  function watchAutoPlay() {";
		script += "    var tries = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      tries++;";
		script += "      var videos = document.querySelectorAll('video');";
		script += "      for (var i = 0; i < videos.length; i++) {";
		script += "        var v = videos[i];";
		script += "        if (v.paused && v.readyState >= 3) {";
		script += "          clearInterval(iv);";
		script += "          v.play().catch(function() {";
		script += "            var overlay = document.querySelector('.vjs-big-play-button, [class*=\"play-overlay\"], [class*=\"play-button\"]');";
		script += "            if (overlay && overlay.offsetParent !== null) overlay.click();";
		script += "          });";
		script += "          return;";
		script += "        }";
		script += "      }";
		script += "      var overlay = document.querySelector('.vjs-big-play-button, [class*=\"play-overlay\"], [class*=\"play-button\"]');";
		script += "      if (overlay && overlay.offsetParent !== null) { clearInterval(iv); overlay.click(); return; }";
		script += "      if (tries > 20) clearInterval(iv);";
		script += "    }, 500);";
		script += "  }";
		script += "  function openClip(clipId, sportId) {";
		script += "    console.error('[obs-betfair] abrindo clipId=' + clipId + ' sportId=' + sportId);";
		script += "    if (typeof selectClip === 'function') {";
		script += "      selectClip(String(clipId));";
		script += "      sendARequest('EVENT', sportId);";
		script += "      setTimeout(watchAutoPlay, 1500);";
		script += "    } else { console.error('[obs-betfair] selectClip nao encontrado na pagina'); }";
		script += "  }";
		script += "  function schedule() {";
		script += "    var now = Date.now();";
		script += "    var best = null;";
		script += "    var allClips = document.querySelectorAll('.clip');";
		script += "    var withLink = 0, matched = 0;";
		script += "    allClips.forEach(function(clip) {";
		script += "      var a = clip.querySelector('.clip-bet a');";
		script += "      if (!a) return;";
		script += "      withLink++;";
		script += "      var mm = a.href.match(/market\\/([\\d.]+)/);";
		script += "      if (!mm) return;";
		script += "      var mid = mm[1];";
		script += "      if (marketIds.indexOf(mid) !== -1) matched++;";
		script += "      if (marketIds.indexOf(mid) === -1 || handled[mid]) return;";
		script += "      var idm = clip.id.match(/clipId_(\\d+)_sportId_(\\d+)/);";
		script += "      if (!idm) return;";
		script += "      var cid = parseInt(idm[1]), sid = parseInt(idm[2]);";
		script += "      var spans = clip.querySelectorAll('.clip-info-inner > span');";
		script += "      var sp = spans[spans.length - 1];";
		script += "      if (!sp) return;";
		script += "      var t = parseTime(sp.textContent.trim());";
		script += "      if (!t) { console.error('[obs-betfair] parseTime falhou para: ' + sp.textContent.trim()); return; }";
		script += "      handled[mid] = true;";
		script += "      var delay = t.getTime() - 120000 - now;";
		script += "      if (delay <= 0) {";
		script += "        if (!best || t.getTime() > best.t) best = {cid:cid, sid:sid, t:t.getTime()};";
		script += "      } else {";
		script += "        console.error('[obs-betfair] agendado: market=' + mid + ' horario=' + t.toLocaleTimeString() + ' delay=' + Math.round(delay/60000) + 'min');";
		script += "        (function(c,s) { setTimeout(function() { openClip(c,s); }, delay); })(cid,sid);";
		script += "      }";
		script += "    });";
		script += "    console.error('[obs-betfair] scan: ' + allClips.length + ' clips, ' + withLink + ' com link aposta, ' + matched + ' match(es) encontrado(s)');";
		script += "    if (best) openClip(best.cid, best.sid);";
		script += "  }";
		script += "  function init() {";
		script += "    if (!document.querySelector('.clip')) { setTimeout(init, 1000); return; }";
		script += "    var pending = marketIds.filter(function(id) { return !handled[id]; });";
		script += "    if (pending.length === 0) return;";
		script += "    schedule();";
		script += "    if (marketIds.some(function(id) { return !handled[id]; })) setTimeout(init, 120000);";
		script += "  }";
		script += "  setTimeout(init, 2000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->superbet_username.length() && bs->superbet_password.length()) {
		std::string user = CefURIEncode(bs->superbet_username, false).ToString();
		std::string pass = CefURIEncode(bs->superbet_password, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsSuperbetLogin) return;";
		script += "  window.__obsSuperbetLogin = true;";
		script += "  if (window.location.hostname.indexOf('superbet') === -1) return;";
		script += "  if (window.location.pathname.indexOf('/odds/') !== -1) return;";
		script += "  var u = decodeURIComponent('" + user + "');";
		script += "  var p = decodeURIComponent('" + pass + "');";
		script += "  var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;";
		script += "  function fillInput(el, val) {";
		script += "    el.focus();";
		script += "    el.dispatchEvent(new Event('focus', {bubbles:true}));";
		script += "    setter.call(el, val);";
		script += "    el.dispatchEvent(new Event('input', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('change', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('keyup', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('blur', {bubbles:true}));";
		script += "  }";
		script += "  function tryLogin() {";
		script += "    var userEl = document.querySelector('input[name=\"username\"]');";
		script += "    var passEl = document.querySelector('input[name=\"password\"]');";
		script += "    if (!userEl || !passEl || userEl.offsetParent === null) return false;";
		script += "    fillInput(userEl, u);";
		script += "    setTimeout(function() {";
		script += "      fillInput(passEl, p);";
		script += "      setTimeout(function() {";
		script += "        var btn = document.querySelector('.e2e-login-submit-btn');";
		script += "        if (!btn) return;";
		script += "        btn.click();";
		script += "        setTimeout(function() { if (window.__obsSuperbetTriggerSched) window.__obsSuperbetTriggerSched(); }, 3000);";
		script += "      }, 400);";
		script += "    }, 300);";
		script += "    return true;";
		script += "  }";
		script += "  var attempts = 0;";
		script += "  var _lastEntrarClick = 0;";
		script += "  function poll() {";
		script += "    if (tryLogin()) return;";
		script += "    var now = Date.now();";
		script += "    if (now - _lastEntrarClick > 2000) {";
		script += "      var entrar = document.querySelector('.e2e-login');";
		script += "      if (entrar) { entrar.click(); _lastEntrarClick = now; }";
		script += "    }";
		script += "    attempts++;";
		script += "    if (attempts < 30) setTimeout(poll, 1000);";
		script += "  }";
		script += "  setTimeout(poll, 500);";
		script += "  setInterval(function() {";
		script += "    var userEl = document.querySelector('input[name=\"username\"]');";
		script += "    if (userEl && userEl.offsetParent !== null) tryLogin();";
		script += "  }, 15000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->superbet_event_ids.length()) {
		std::string encoded = CefURIEncode(bs->superbet_event_ids, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsSuperbetSched) return;";
		script += "  window.__obsSuperbetSched = true;";
		script += "  if (window.location.hostname.indexOf('superbet') === -1) return;";
		script += "  var raw = decodeURIComponent('" + encoded + "');";
		script += "  var eventIds = raw.split(/[\\n\\r,]+/).map(function(s) { return s.trim(); }).filter(Boolean);";
		script += "  if (!eventIds.length) return;";
		script += "  function slugify(s) {";
		script += "    return s.toLowerCase()";
		script += "      .normalize('NFD').replace(/[\\u0300-\\u036f]/g, '')";
		script += "      .replace(/[^a-z0-9]+/g, '-')";
		script += "      .replace(/^-|-$/g, '');";
		script += "  }";
		script += "  function buildUrl(ev) {";
		script += "    var parts = ev.fixture.event_name.split('\\xb7');";
		script += "    var home = slugify(parts[0].trim());";
		script += "    var away = slugify(parts[1] ? parts[1].trim() : '');";
		script += "    return 'https://superbet.bet.br/odds/futebol/' + home + '-x-' + away + '-' + ev.event_id + '/?t=P-superLive&mdt=o';";
		script += "  }";
		script += "  function watchPerformIframe() {";
		script += "    var tries = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      tries++;";
		script += "      var frames = document.querySelectorAll('iframe');";
		script += "      var found = null;";
		script += "      for (var f = 0; f < frames.length; f++) {";
		script += "        var src = frames[f].src || '';";
		script += "        if (src && src.indexOf('superbet') === -1 && src.length > 10) { found = src; break; }";
		script += "      }";
		script += "      if (found) {";
		script += "        clearInterval(iv);";
		script += "        found = found.replace(/[&?]width=\\d+/, '').replace(/[&?]used=true/, '');";
		script += "        var sep = found.indexOf('?') !== -1 ? '&' : '?';";
		script += "        window.location.href = found + sep + '__obs_player=1';";
		script += "      }";
		script += "      if (tries > 20) clearInterval(iv);";
		script += "    }, 1000);";
		script += "  }";
		script += "  function clickAssista() {";
		script += "    var tries = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      tries++;";
		script += "      var labels = document.querySelectorAll('.e2e-sds-button__label');";
		script += "      for (var i = 0; i < labels.length; i++) {";
		script += "        if (labels[i].textContent.trim() === 'Assista') {";
		script += "          var ab = labels[i].closest ? labels[i].closest('button') : labels[i].parentElement;";
		script += "          if (ab) { clearInterval(iv); ab.click(); setTimeout(watchPerformIframe, 2000); return; }";
		script += "        }";
		script += "      }";
		script += "      if (tries > 20) clearInterval(iv);";
		script += "    }, 500);";
		script += "  }";
		script += "  function keepAlive() {";
		script += "    var btns = document.querySelectorAll('button, .sds-button, .e2e-sds-button__label');";
		script += "    for (var i = 0; i < btns.length; i++) {";
		script += "      var t = btns[i].textContent.trim();";
		script += "      if (t === 'Volte a jogar' || t === 'Continue' || t === 'Continuar') {";
		script += "        var el = btns[i].closest ? btns[i].closest('button') : btns[i];";
		script += "        if (el) { el.click(); break; }";
		script += "      }";
		script += "    }";
		script += "    document.dispatchEvent(new MouseEvent('mousemove', {bubbles:true, clientX: Math.random()*100, clientY: Math.random()*100}));";
		script += "  }";
		script += "  setInterval(keepAlive, 30000);";
		script += "  function clickVideoBtn() {";
		script += "    var tries = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      tries++;";
		script += "      var labels = document.querySelectorAll('.sds-button-text__label');";
		script += "      for (var i = 0; i < labels.length; i++) {";
		script += "        if (labels[i].textContent.trim() === 'Transmissão') {";
		script += "          var btn = labels[i].closest ? labels[i].closest('button') : labels[i].parentElement;";
		script += "          if (btn) { clearInterval(iv); btn.click(); setTimeout(clickAssista, 1000); return; }";
		script += "        }";
		script += "      }";
		script += "      if (tries > 30) clearInterval(iv);";
		script += "    }, 500);";
		script += "  }";
		script += "  if (window.location.pathname.indexOf('/odds/futebol/') !== -1) {";
		script += "    setTimeout(clickVideoBtn, 1000);";
		script += "    return;";
		script += "  }";
		script += "  var handled = {};";
		script += "  function fetchAndSchedule() {";
		script += "    fetch('https://production-superbet-offer-br.freetls.fastly.net/v3/subscription/pt-BR/live?sports=5')";
		script += "      .then(function(r) { return r.text(); })";
		script += "      .then(function(t) {";
		script += "        var m = t.match(/^data:(.+)/m);";
		script += "        if (!m) return;";
		script += "        var events = JSON.parse(m[1]);";
		script += "        var now = Date.now();";
		script += "        var best = null;";
		script += "        events.forEach(function(ev) {";
		script += "          if (!ev.fixture || !ev.fixture.streams || !Object.keys(ev.fixture.streams).length) return;";
		script += "          var idStr = String(ev.event_id);";
		script += "          if (eventIds.indexOf(idStr) === -1) return;";
		script += "          if (handled[idStr]) return;";
		script += "          var startMs = new Date(ev.fixture.utc_date).getTime();";
		script += "          var delay = startMs - 120000 - now;";
		script += "          handled[idStr] = true;";
		script += "          if (delay <= 0) {";
		script += "            if (!best || startMs > best.startMs) best = { ev: ev, startMs: startMs };";
		script += "          } else {";
		script += "            console.error('[obs-superbet] agendado id=' + idStr + ' delay=' + Math.round(delay/60000) + 'min');";
		script += "            (function(e) { setTimeout(function() { window.location.href = buildUrl(e); }, delay); })(ev);";
		script += "          }";
		script += "        });";
		script += "        if (best) {";
		script += "          console.error('[obs-superbet] abrindo imediato id=' + best.ev.event_id);";
		script += "          window.location.href = buildUrl(best.ev);";
		script += "        }";
		script += "      })";
		script += "      .catch(function(e) { console.error('[obs-superbet] erro: ' + e); });";
		script += "  }";
		script += "  window.__obsSuperbetTriggerSched = function() { handled = {}; fetchAndSchedule(); };";
		script += "  fetchAndSchedule();";
		script += "  setInterval(fetchAndSchedule, 15000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->bet365_username.length() && bs->bet365_password.length()) {
		std::string user = CefURIEncode(bs->bet365_username, false).ToString();
		std::string pass = CefURIEncode(bs->bet365_password, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsBet365Login) return;";
		script += "  window.__obsBet365Login = true;";
		script += "  if (window.location.hostname.indexOf('bet365') === -1) return;";
		script += "  var u = decodeURIComponent('" + user + "');";
		script += "  var p = decodeURIComponent('" + pass + "');";
		script += "  var setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;";
		script += "  function fillInput(el, val) {";
		script += "    el.focus();";
		script += "    el.dispatchEvent(new Event('focus', {bubbles:true}));";
		script += "    setter.call(el, val);";
		script += "    el.dispatchEvent(new Event('input', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('change', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('keyup', {bubbles:true}));";
		script += "    el.dispatchEvent(new Event('blur', {bubbles:true}));";
		script += "  }";
		script += "  function tryLogin() {";
		script += "    var passEl = document.querySelector('input[type=\"password\"]');";
		script += "    if (!passEl || passEl.offsetParent === null) return false;";
		script += "    var userEl = null;";
		script += "    var inputs = document.querySelectorAll('input[type=\"text\"], input[type=\"email\"], input:not([type])');";
		script += "    for (var i = 0; i < inputs.length; i++) {";
		script += "      if (inputs[i].offsetParent !== null) { userEl = inputs[i]; break; }";
		script += "    }";
		script += "    if (!userEl) return false;";
		script += "    fillInput(userEl, u);";
		script += "    setTimeout(function() {";
		script += "      fillInput(passEl, p);";
		script += "      setTimeout(function() {";
		script += "        var btn = null;";
		script += "        var btns = document.querySelectorAll('button');";
		script += "        for (var b = 0; b < btns.length; b++) {";
		script += "          if (/login|entrar|submit|log in/i.test(btns[b].textContent)) { btn = btns[b]; break; }";
		script += "        }";
		script += "        if (!btn) btn = document.querySelector('input[type=\"submit\"]');";
		script += "        if (btn) {";
		script += "          btn.click();";
		script += "          setTimeout(function() { if (window.__obsBet365TriggerSched) window.__obsBet365TriggerSched(); }, 3000);";
		script += "        }";
		script += "      }, 400);";
		script += "    }, 300);";
		script += "    return true;";
		script += "  }";
		script += "  var attempts = 0;";
		script += "  function poll() {";
		script += "    if (tryLogin()) return;";
		script += "    attempts++;";
		script += "    if (attempts < 30) setTimeout(poll, 1000);";
		script += "  }";
		script += "  setTimeout(poll, 1000);";
		script += "  setInterval(function() {";
		script += "    var passEl = document.querySelector('input[type=\"password\"]');";
		script += "    if (passEl && passEl.offsetParent !== null) tryLogin();";
		script += "  }, 15000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain() && bs->bet365_event_ids.length()) {
		std::string encoded = CefURIEncode(bs->bet365_event_ids, false).ToString();

		std::string script;
		script += "(function() {";
		script += "  if (window.__obsBet365Sched) return;";
		script += "  window.__obsBet365Sched = true;";
		script += "  if (window.location.hostname.indexOf('bet365') === -1) return;";
		script += "  var raw = decodeURIComponent('" + encoded + "');";
		script += "  var eventIds = raw.split(/[\\n\\r,]+/).map(function(s) { return s.trim(); }).filter(Boolean);";
		script += "  if (!eventIds.length) return;";
		script += "  function getPageEventId() {";
		script += "    var h = window.location.hash || '';";
		script += "    var m = h.match(/\\/E\\/([0-9]+)\\//);";
		script += "    return m ? m[1] : null;";
		script += "  }";
		script += "  function watchIframe() {";
		script += "    var tries = 0;";
		script += "    var iv = setInterval(function() {";
		script += "      tries++;";
		script += "      var frames = document.querySelectorAll('iframe');";
		script += "      var found = null;";
		script += "      for (var f = 0; f < frames.length; f++) {";
		script += "        var src = frames[f].src || '';";
		script += "        if (src && src.indexOf('bet365') === -1 && src.length > 10) { found = src; break; }";
		script += "      }";
		script += "      if (found) {";
		script += "        clearInterval(iv);";
		script += "        found = found.replace(/[&?]width=\\d+/, '').replace(/[&?]used=true/, '');";
		script += "        var sep = found.indexOf('?') !== -1 ? '&' : '?';";
		script += "        window.location.href = found + sep + '__obs_player=1';";
		script += "      }";
		script += "      if (tries > 30) clearInterval(iv);";
		script += "    }, 1000);";
		script += "  }";
		script += "  function clickWatchBtn() {";
		script += "    var sels = [";
		script += "      '.sip-LiveStreamingButton', '[class*=\"LiveStream\"]', '[class*=\"WatchLive\"]',";
		script += "      '[class*=\"lv-Video\"]', '[class*=\"lv-Watch\"]', '.tv-Icon',";
		script += "      '[aria-label*=\"Watch\"]', '[title*=\"Watch\"]', '[title*=\"Assistir\"]'";
		script += "    ];";
		script += "    for (var s = 0; s < sels.length; s++) {";
		script += "      var els = document.querySelectorAll(sels[s]);";
		script += "      for (var i = 0; i < els.length; i++) {";
		script += "        if (els[i].offsetParent !== null) { els[i].click(); setTimeout(watchIframe, 2000); return true; }";
		script += "      }";
		script += "    }";
		script += "    return false;";
		script += "  }";
		script += "  var handled = {};";
		script += "  function checkAndOpen() {";
		script += "    var pageId = getPageEventId();";
		script += "    if (pageId && eventIds.indexOf(pageId) !== -1) {";
		script += "      if (!handled[pageId]) {";
		script += "        setTimeout(function() { if (clickWatchBtn()) handled[pageId] = true; }, 2000);";
		script += "      }";
		script += "      return;";
		script += "    }";
		script += "    for (var i = 0; i < eventIds.length; i++) {";
		script += "      if (!handled[eventIds[i]]) {";
		script += "        window.location.href = 'https://www.bet365.com.br/en/#/IP/E/E/' + eventIds[i] + '/';";
		script += "        return;";
		script += "      }";
		script += "    }";
		script += "  }";
		script += "  window.__obsBet365TriggerSched = function() { handled = {}; checkAndOpen(); };";
		script += "  checkAndOpen();";
		script += "  setInterval(checkAndOpen, 30000);";
		script += "})();";

		frame->ExecuteJavaScript(script, "", 0);
	}

	if (frame->IsMain()) {
		std::string frameUrl = frame->GetURL().ToString();
		if (frameUrl.find("performgroup.com") != std::string::npos ||
		    frameUrl.find("__obs_player=1") != std::string::npos) {
			std::string script;
			script += "(function() {";
			script += "  if (window.__obsPerformFull) return;";
			script += "  window.__obsPerformFull = true;";
			script += "  var maximized = false;";
			script += "  var maxSelectors = [";
			script += "    'button[title*=\"Full\"]', 'button[title*=\"full\"]',";
			script += "    'button[aria-label*=\"full\" i]', 'button[aria-label*=\"maximiz\" i]',";
			script += "    '.fullscreen-button', '.vjs-fullscreen-control',";
			script += "    'button[data-id=\"fullscreen\"]', '[class*=\"fullscreen\"]'";
			script += "  ];";
			script += "  function tryMaxBtn() {";
			script += "    for (var s = 0; s < maxSelectors.length; s++) {";
			script += "      try {";
			script += "        var btn = document.querySelector(maxSelectors[s]);";
			script += "        if (btn && (btn.tagName === 'BUTTON' || btn.getAttribute('role') === 'button')) {";
			script += "          btn.click(); maximized = true; return true;";
			script += "        }";
			script += "      } catch(e) {}";
			script += "    }";
			script += "    return false;";
			script += "  }";
			script += "  function fill() {";
			script += "    if (maximized) return;";
			script += "    if (tryMaxBtn()) return;";
			script += "    var s = document.documentElement.style;";
			script += "    s.margin = '0'; s.padding = '0'; s.overflow = 'hidden';";
			script += "    s.width = '100%'; s.height = '100%'; s.background = '#000';";
			script += "    var bs = document.body ? document.body.style : null;";
			script += "    if (bs) { bs.margin='0'; bs.padding='0'; bs.overflow='hidden'; bs.width='100%'; bs.height='100%'; bs.background='#000'; }";
			script += "    var vids = document.querySelectorAll('video');";
			script += "    for (var i = 0; i < vids.length; i++) {";
			script += "      vids[i].style.cssText = 'position:fixed!important;top:0!important;left:0!important;width:100vw!important;height:100vh!important;object-fit:contain!important;z-index:9999!important;background:#000!important;';";
			script += "    }";
			script += "    var divs = document.querySelectorAll('div');";
			script += "    for (var j = 0; j < divs.length; j++) {";
			script += "      var c = divs[j].className || '';";
			script += "      if (typeof c === 'string' && (c.indexOf('player') !== -1 || c.indexOf('video') !== -1 || c.indexOf('stream') !== -1)) {";
			script += "        divs[j].style.cssText = 'width:100vw!important;height:100vh!important;max-width:none!important;max-height:none!important;position:fixed!important;top:0!important;left:0!important;';";
			script += "      }";
			script += "    }";
			script += "  }";
			script += "  fill();";
			script += "  setTimeout(fill, 500);";
			script += "  setTimeout(fill, 2000);";
			script += "  setTimeout(fill, 4000);";
			script += "  var mo = new MutationObserver(function() { fill(); });";
			script += "  setTimeout(function() { if (document.body) mo.observe(document.body, {childList:true, subtree:true}); }, 200);";
			script += "})();";

			frame->ExecuteJavaScript(script, "", 0);
		}
	}

	if (frame->IsMain() && bs->css.length()) {
		std::string uriEncodedCSS = CefURIEncode(bs->css, false).ToString();

		std::string script;
		script += "const obsCSS = document.createElement('style');";
		script += "obsCSS.appendChild(document.createTextNode("
			  "decodeURIComponent(\"" +
			  uriEncodedCSS + "\")));";
		script += "document.querySelector('head').appendChild(obsCSS);";

		frame->ExecuteJavaScript(script, "", 0);
	}
}

bool BrowserClient::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString &message,
				     const CefString &source, int line)
{
	int errorLevel = LOG_INFO;
	const char *code = "Info";
	switch (level) {
	case LOGSEVERITY_ERROR:
		errorLevel = LOG_WARNING;
		code = "Error";
		break;
	case LOGSEVERITY_FATAL:
		errorLevel = LOG_ERROR;
		code = "Fatal";
		break;
	default:
		return false;
	}

	const char *sourceName = "<unknown>";

	if (bs && bs->source)
		sourceName = obs_source_get_name(bs->source);

	blog(errorLevel, "[obs-browser: '%s'] %s: %s (%s:%d)", sourceName, code, message.ToString().c_str(),
	     source.ToString().c_str(), line);
	return false;
}
