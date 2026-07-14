#include "winrt-capture.h"

extern "C" EXPORT BOOL winrt_capture_supported()
try {
	/* no contract for IGraphicsCaptureItemInterop, verify 10.0.18362.0 */
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
		L"Windows.Foundation.UniversalApiContract", 8);
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

extern "C" EXPORT BOOL winrt_capture_cursor_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_cursor_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

template<typename T>
static winrt::com_ptr<T> GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const &object)
{
	auto access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
	winrt::com_ptr<T> result;
	winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
	return result;
}

static bool get_client_box(HWND window, uint32_t width, uint32_t height, D3D11_BOX *client_box)
{
	RECT client_rect{}, window_rect{};
	POINT upper_left{};

	/* check iconic (minimized) twice, ABA is very unlikely */
	bool client_box_available = !IsIconic(window) && GetClientRect(window, &client_rect) && !IsIconic(window) &&
				    (client_rect.right > 0) && (client_rect.bottom > 0) &&
				    (DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &window_rect,
							   sizeof(window_rect)) == S_OK) &&
				    ClientToScreen(window, &upper_left);
	if (client_box_available) {
		const uint32_t left = (upper_left.x > window_rect.left) ? (upper_left.x - window_rect.left) : 0;
		client_box->left = left;

		const uint32_t top = (upper_left.y > window_rect.top) ? (upper_left.y - window_rect.top) : 0;
		client_box->top = top;

		uint32_t texture_width = 1;
		if (width > left) {
			texture_width = min(width - left, (uint32_t)client_rect.right);
		}

		uint32_t texture_height = 1;
		if (height > top) {
			texture_height = min(height - top, (uint32_t)client_rect.bottom);
		}

		client_box->right = left + texture_width;
		client_box->bottom = top + texture_height;

		client_box->front = 0;
		client_box->back = 1;

		client_box_available = (client_box->right <= width) && (client_box->bottom <= height);
	}

	return client_box_available;
}

static DXGI_FORMAT get_pixel_format(HWND window, HMONITOR monitor, BOOL force_sdr)
{
	static constexpr DXGI_FORMAT sdr_format = DXGI_FORMAT_B8G8R8A8_UNORM;

	if (force_sdr)
		return sdr_format;

	if (window)
		monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);

	return (monitor && gs_is_monitor_hdr(monitor)) ? DXGI_FORMAT_R16G16B16A16_FLOAT : sdr_format;
}

static inline D3D11_BOX RECT_to_D3D11_BOX(const RECT *rect_ptr)
{
	D3D11_BOX box = {0, 0, 0, 0, 0, 0}; // Zero initialize
	if (rect_ptr) {
		box.left = (UINT)rect_ptr->left;
		box.top = (UINT)rect_ptr->top;
		box.front = 0;
		box.right = (UINT)rect_ptr->right;
		box.bottom = (UINT)rect_ptr->bottom;
		box.back = 1;
	}
	return box;
}

struct winrt_capture {
	HWND window;
	BOOL client_area;
	BOOL force_sdr;
	HMONITOR monitor;
	DXGI_FORMAT format;

	BOOL use_subregion;
	RECT sub_rect;

	bool capture_cursor;
	BOOL cursor_visible;

	gs_texture_t *texture;
	bool texture_written;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
	winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{nullptr};
	ComPtr<ID3D11DeviceContext> context;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool{nullptr};
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
	winrt::Windows::Graphics::SizeInt32 last_size;
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem::Closed_revoker closed;
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker frame_arrived;

	uint32_t texture_width;
	uint32_t texture_height;
	D3D11_BOX client_box;

	BOOL active;
	struct winrt_capture *next;

	void on_closed(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const &,
		       winrt::Windows::Foundation::IInspectable const &)
	{
		active = FALSE;
	}

	void on_frame_arrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const &sender,
			      winrt::Windows::Foundation::IInspectable const &)
	{
		const winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame = sender.TryGetNextFrame();
		const winrt::Windows::Graphics::SizeInt32 frame_content_size = frame.ContentSize();

		winrt::com_ptr<ID3D11Texture2D> frame_surface =
			GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());

		/* need GetDesc because ContentSize is not reliable */
		D3D11_TEXTURE2D_DESC desc;
		frame_surface->GetDesc(&desc);

		obs_enter_graphics();

		if (desc.Format == get_pixel_format(window, monitor, force_sdr)) {
			// 원본 코드의 지역 변수 선언 유지 (get_client_box가 D3D11_BOX*를 받는다고 가정)
			bool current_frame_use_subregion = use_subregion;
			RECT current_frame_actual_sub_rect = {0, 0, 0, 0}; // 실제 적용될 sub_rect (클램핑 후)

			if (!client_area ||
			    get_client_box(window, desc.Width, desc.Height, &client_box)) {
				if (current_frame_use_subregion) {
					current_frame_actual_sub_rect =sub_rect;

					// 현재 프레임(desc) 크기에 맞게 sub_rect 클램핑
					if (current_frame_actual_sub_rect.left < 0)
						current_frame_actual_sub_rect.left = 0;
					if (current_frame_actual_sub_rect.top < 0)
						current_frame_actual_sub_rect.top = 0;
					if (current_frame_actual_sub_rect.right > (LONG)desc.Width)
						current_frame_actual_sub_rect.right = (LONG)desc.Width;
					if (current_frame_actual_sub_rect.bottom > (LONG)desc.Height)
						current_frame_actual_sub_rect.bottom = (LONG)desc.Height;

					texture_width = current_frame_actual_sub_rect.right - current_frame_actual_sub_rect.left;
					texture_height = current_frame_actual_sub_rect.bottom - current_frame_actual_sub_rect.top;

					if (!(texture_width > 0 && texture_height > 0)) {
						blog(LOG_WARNING, "Subregion for window %p resulted in zero size after clamping. Original: (%ld,%ld)-(%ld,%ld), Frame: %ux%u. Fallback to original logic.",
						     window, sub_rect.left, sub_rect.top, sub_rect.right, sub_rect.bottom, desc.Width, desc.Height);
						current_frame_use_subregion = false; // 이 프레임에 대해서는 subregion 사용 안함 (원본 로직으로 대체)
					}
				} else {
					if (client_area) {
						texture_width = client_box.right - client_box.left;
						texture_height = client_box.bottom - client_box.top;
					} else {
						texture_width = desc.Width;
						texture_height = desc.Height;
					}
				}

				if (texture) {
					if (texture_width != gs_texture_get_width(texture) ||
					    texture_height != gs_texture_get_height(texture)) {
						gs_texture_destroy(texture);
						texture = nullptr;
					}
				}

				if (!texture) {
					const gs_color_format color_format =
						desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? GS_RGBA16F : GS_BGRA;
					texture = gs_texture_create(texture_width, texture_height, color_format, 1, NULL, 0);
				}

				if (texture) { // texture가 유효한 경우에만 복사
					ID3D11Resource *obs_dx_texture_rsc = (ID3D11Resource *)gs_texture_get_obj(texture);
					if (obs_dx_texture_rsc) {
						if (current_frame_use_subregion) { // subregion 사용 시 CopySubresourceRegion
							D3D11_BOX d3d_source_sub_rect_box =
								RECT_to_D3D11_BOX(
									&current_frame_actual_sub_rect);
							context->CopySubresourceRegion(
								obs_dx_texture_rsc,
								0, 0, 0, 0,
								frame_surface
									.get(),
								0,
								&d3d_source_sub_rect_box);
							texture_written = true;
						} else { // subregion 미사용 시 원본 복사 로직
							if (client_area) { // client_box 지역 변수 사용 (D3D11_BOX 타입으로 가정)
								context->CopySubresourceRegion(
									obs_dx_texture_rsc,
									0, 0, 0,
									0,
									frame_surface
										.get(),
									0,
									&client_box); // get_client_box가 채운 D3D11_BOX 사용
								texture_written = true;
							} else { // 전체 프레임
								context->CopyResource(
									obs_dx_texture_rsc,
									frame_surface
										.get());
								texture_written = true;
							}
						}
					} else {
						blog(LOG_ERROR, "on_frame_arrived: Failed to get D3D resource from OBS texture for window %p.", window);
						texture_written = false;
					}
				} else { // this->texture가 NULL (생성 실패 또는 크기가 0)
					texture_written = false;
				}
			} else { // 원본 if (!this->client_area || get_client_box(...)) 조건이 false인 경우 (client_area=true인데 get_client_box 실패)
				blog(LOG_WARNING, "on_frame_arrived: Client area for window %p true, but get_client_box failed. No texture written.", window);
				if (texture) { // 기존 텍스처가 있다면 파괴
					gs_texture_destroy(texture);
					texture = nullptr;
				}
				texture_written = false;
			}

			// 프레임 풀 재생성 로직 (원본 코드와 동일, 멤버 변수 접근 시 this-> 사용)
			if (frame_content_size.Width != last_size.Width ||
			    frame_content_size.Height != last_size.Height) {
				format = desc.Format;
				frame_pool.Recreate(
					device,
					static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2,
					frame_content_size);

				last_size = frame_content_size;
			}
		} else { // 프레임 포맷 불일치
			blog(LOG_WARNING, "on_frame_arrived: WGC frame format mismatch for window %p. Expected: %d, Got: %d.",
			     window, format, desc.Format);
			if (texture) {
				gs_texture_destroy(texture);
				texture = nullptr;
			}
			texture_written = false;
			active = FALSE; // 멤버 active
		}

		obs_leave_graphics();
	}
};

static struct winrt_capture *capture_list;

static void winrt_capture_device_loss_release(void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);
	capture->active = FALSE;

	capture->frame_arrived.revoke();

	try {
		capture->frame_pool.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
	}

	try {
		capture->session.Close();
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
	}

	capture->session = nullptr;
	capture->frame_pool = nullptr;
	capture->context = nullptr;
	capture->device = nullptr;
	capture->item = nullptr;
}

static bool winrt_capture_border_toggle_supported()
try {
	return winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
		L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X): %s", err.code().value,
	     winrt::to_string(err.message()).c_str());
	return false;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_border_toggle_supported (0x%08X)", winrt::to_hresult().value);
	return false;
}

static winrt::Windows::Graphics::Capture::GraphicsCaptureItem
winrt_capture_create_item(IGraphicsCaptureItemInterop *const interop_factory, HWND window, HMONITOR monitor)
{
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item = {nullptr};
	if (window) {
		try {
			const HRESULT hr = interop_factory->CreateForWindow(
				window, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr))
				blog(LOG_ERROR, "CreateForWindow (0x%08X)", hr);
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForWindow (0x%08X)", winrt::to_hresult().value);
		}
	} else {
		assert(monitor);

		try {
			const HRESULT hr = interop_factory->CreateForMonitor(
				monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
				reinterpret_cast<void **>(winrt::put_abi(item)));
			if (FAILED(hr))
				blog(LOG_ERROR, "CreateForMonitor (0x%08X)", hr);
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "CreateForMonitor (0x%08X)", winrt::to_hresult().value);
		}
	}

	return item;
}

static void winrt_capture_device_loss_rebuild(void *device_void, void *data)
{
	winrt_capture *capture = static_cast<winrt_capture *>(data);

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), capture->window, capture->monitor);
	if (!item)
		return;

	ID3D11Device *const d3d_device = (ID3D11Device *)device_void;
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(d3d_device->QueryInterface(&dxgi_device)))
		blog(LOG_ERROR, "Failed to get DXGI device");

	winrt::com_ptr<IInspectable> inspectable;
	if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put())))
		blog(LOG_ERROR, "Failed to get WinRT device");

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(capture->format), 2,
			capture->last_size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	if (winrt_capture_cursor_toggle_supported())
		session.IsCursorCaptureEnabled(capture->capture_cursor && capture->cursor_visible);

	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->frame_arrived =
		frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});

	try {
		session.StartCapture();
		capture->active = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "StartCapture (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "StartCapture (0x%08X)", winrt::to_hresult().value);
	}
}

static struct winrt_capture *winrt_capture_init_internal(BOOL cursor, HWND window, BOOL client_area, BOOL force_sdr,
	 												     HMONITOR monitor, BOOL use_subregion, const RECT *subregion_rect)
try {
	ID3D11Device *const d3d_device = (ID3D11Device *)gs_get_device_obj();
	ComPtr<IDXGIDevice> dxgi_device;

	HRESULT hr = d3d_device->QueryInterface(&dxgi_device);
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get DXGI device");
		return nullptr;
	}

	winrt::com_ptr<IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), inspectable.put());
	if (FAILED(hr)) {
		blog(LOG_ERROR, "Failed to get WinRT device");
		return nullptr;
	}

	auto activation_factory =
		winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
	auto interop_factory = activation_factory.as<IGraphicsCaptureItemInterop>();
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem item =
		winrt_capture_create_item(interop_factory.get(), window, monitor);
	if (!item)
		return nullptr;

	const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device =
		inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
	const winrt::Windows::Graphics::SizeInt32 size = item.Size();
	const DXGI_FORMAT format = get_pixel_format(window, monitor, force_sdr);
	const winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool frame_pool =
		winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
			device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(format), 2, size);
	const winrt::Windows::Graphics::Capture::GraphicsCaptureSession session = frame_pool.CreateCaptureSession(item);

	if (winrt_capture_border_toggle_supported()) {
		winrt::Windows::Graphics::Capture::GraphicsCaptureAccess::RequestAccessAsync(
			winrt::Windows::Graphics::Capture::GraphicsCaptureAccessKind::Borderless)
			.get();
		session.IsBorderRequired(false);
	}

	/* disable cursor capture if possible since ours performs better */
	const BOOL cursor_toggle_supported = winrt_capture_cursor_toggle_supported();
	if (cursor_toggle_supported)
		session.IsCursorCaptureEnabled(cursor);

	struct winrt_capture *capture = new winrt_capture{};
	capture->window = window;
	capture->client_area = client_area;
	capture->force_sdr = force_sdr;
	capture->monitor = monitor;
	capture->format = format;
	capture->capture_cursor = cursor && cursor_toggle_supported;
	capture->cursor_visible = cursor;
	capture->item = item;
	capture->device = device;
	d3d_device->GetImmediateContext(&capture->context);
	capture->frame_pool = frame_pool;
	capture->session = session;
	capture->last_size = size;
	capture->use_subregion = use_subregion;
	if (use_subregion && subregion_rect) {
		capture->sub_rect = *subregion_rect; // 전달받은 RECT 값 복사

		// 하위 영역 유효성 검사 및 조정 (매우 중요)
		// crop_rect가 size 내에 있는지, 너비/높이가 양수인지 확인
		if (capture->sub_rect.left < 0)
			capture->sub_rect.left = 0;
		if (capture->sub_rect.top < 0)
			capture->sub_rect.top = 0;
		// right, bottom은 좌표값이므로 size의 너비/높이 값과 비교
		if (capture->sub_rect.right > size.Width)
			capture->sub_rect.right = size.Width;
		if (capture->sub_rect.bottom > size.Height)
			capture->sub_rect.bottom = size.Height;

		// 조정 후 너비/높이가 유효한지 최종 확인
		if ((capture->sub_rect.right - capture->sub_rect.left) <= 0 ||
		    (capture->sub_rect.bottom - capture->sub_rect.top) <= 0) {
			blog(LOG_WARNING,
			     "Subregion rectangle is invalid after clamping/validation. Disabling subregion capture for window %p.",
			     window);
			capture->use_subregion = FALSE; // 유효하지 않으면 하위 영역 사용 안 함
		}
	} else {
		// use_subregion이 FALSE이거나 subregion_rect이 NULL인 경우 명시적으로 FALSE 설정
		capture->use_subregion = FALSE;
		// capture->crop_rect는 초기화되지 않아도 use_subregion이 FALSE이므로 사용되지 않음
		// 또는 기본값으로 설정: capture->crop_rect = {0, 0, full_item_size.Width, full_item_size.Height};
	}
	capture->closed = item.Closed(winrt::auto_revoke, {capture, &winrt_capture::on_closed});
	capture->frame_arrived = frame_pool.FrameArrived(winrt::auto_revoke, {capture, &winrt_capture::on_frame_arrived});
	capture->next = capture_list;
	capture_list = capture;

	session.StartCapture();
	capture->active = TRUE;

	gs_device_loss callbacks;
	callbacks.device_loss_release = winrt_capture_device_loss_release;
	callbacks.device_loss_rebuild = winrt_capture_device_loss_rebuild;
	callbacks.data = capture;
	gs_register_loss_callbacks(&callbacks);

	return capture;

} catch (const winrt::hresult_error &err) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X): %s", err.code().value, winrt::to_string(err.message()).c_str());
	return nullptr;
} catch (...) {
	blog(LOG_ERROR, "winrt_capture_init (0x%08X)", winrt::to_hresult().value);
	return nullptr;
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_window(BOOL cursor, HWND window, BOOL client_area, BOOL force_sdr,
	 															  BOOL use_subregion, const RECT *subregion_rect)
{
	return winrt_capture_init_internal(cursor, window, client_area, force_sdr, NULL, use_subregion, subregion_rect);
}

extern "C" EXPORT struct winrt_capture *winrt_capture_init_monitor(BOOL cursor, HMONITOR monitor, BOOL force_sdr,
				   												   BOOL use_subregion, const RECT *subregion_rect)
{
	return winrt_capture_init_internal(cursor, NULL, false, force_sdr, monitor, use_subregion, subregion_rect);
}

extern "C" EXPORT void winrt_capture_free(struct winrt_capture *capture)
{
	if (capture) {
		struct winrt_capture *current = capture_list;
		if (current == capture) {
			capture_list = capture->next;
		} else {
			struct winrt_capture *previous;
			do {
				previous = current;
				current = current->next;
			} while (current != capture);

			previous->next = current->next;
		}

		obs_enter_graphics();
		gs_unregister_loss_callbacks(capture);
		gs_texture_destroy(capture->texture);
		obs_leave_graphics();

		capture->frame_arrived.revoke();
		capture->closed.revoke();

		try {
			if (capture->frame_pool)
				capture->frame_pool.Close();
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "Direct3D11CaptureFramePool::Close (0x%08X)", winrt::to_hresult().value);
		}

		try {
			if (capture->session)
				capture->session.Close();
		} catch (winrt::hresult_error &err) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X): %s", err.code().value,
			     winrt::to_string(err.message()).c_str());
		} catch (...) {
			blog(LOG_ERROR, "GraphicsCaptureSession::Close (0x%08X)", winrt::to_hresult().value);
		}

		delete capture;
	}
}

extern "C" EXPORT BOOL winrt_capture_active(const struct winrt_capture *capture)
{
	return capture->active;
}

extern "C" EXPORT BOOL winrt_capture_show_cursor(struct winrt_capture *capture, BOOL visible)
{
	BOOL success = FALSE;

	try {
		if (capture->capture_cursor) {
			if (capture->cursor_visible != visible) {
				capture->session.IsCursorCaptureEnabled(visible);
				capture->cursor_visible = visible;
			}
		}

		success = TRUE;
	} catch (winrt::hresult_error &err) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X): %s", err.code().value,
		     winrt::to_string(err.message()).c_str());
	} catch (...) {
		blog(LOG_ERROR, "GraphicsCaptureSession::IsCursorCaptureEnabled (0x%08X)", winrt::to_hresult().value);
	}

	return success;
}

extern "C" EXPORT enum gs_color_space winrt_capture_get_color_space(const struct winrt_capture *capture)
{
	return (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? GS_CS_709_EXTENDED : GS_CS_SRGB;
}

extern "C" EXPORT void winrt_capture_render(struct winrt_capture *capture)
{
	if (capture->texture_written) {
		const char *tech_name = "Draw";
		float multiplier = 1.f;
		const gs_color_space current_space = gs_get_color_space();
		if (capture->format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
			switch (current_space) {
			case GS_CS_SRGB:
			case GS_CS_SRGB_16F:
				tech_name = "DrawMultiplyTonemap";
				multiplier = 80.f / obs_get_video_sdr_white_level();
				break;
			case GS_CS_709_EXTENDED:
				tech_name = "DrawMultiply";
				multiplier = 80.f / obs_get_video_sdr_white_level();
			}
		} else if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			multiplier = obs_get_video_sdr_white_level() / 80.f;
		}

		gs_effect_t *const effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_technique_t *tech = gs_effect_get_technique(effect, tech_name);

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		gs_texture_t *const texture = capture->texture;
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

		const size_t passes = gs_technique_begin(tech);
		for (size_t i = 0; i < passes; i++) {
			if (gs_technique_begin_pass(tech, i)) {
				gs_draw_sprite(texture, 0, 0, 0);

				gs_technique_end_pass(tech);
			}
		}
		gs_technique_end(tech);

		gs_blend_state_pop();

		gs_enable_framebuffer_srgb(previous);
	}
}

extern "C" EXPORT uint32_t winrt_capture_width(const struct winrt_capture *capture)
{
	return capture ? capture->texture_width : 0;
}

extern "C" EXPORT uint32_t winrt_capture_height(const struct winrt_capture *capture)
{
	return capture ? capture->texture_height : 0;
}

extern "C" EXPORT void winrt_capture_thread_start()
{
	struct winrt_capture *capture = capture_list;
	void *const device = gs_get_device_obj();
	while (capture) {
		winrt_capture_device_loss_rebuild(device, capture);
		capture = capture->next;
	}
}

extern "C" EXPORT void winrt_capture_thread_stop()
{
	struct winrt_capture *capture = capture_list;
	while (capture) {
		winrt_capture_device_loss_release(capture);
		capture = capture->next;
	}
}
