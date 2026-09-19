#include "Wayland.h"


extern const struct wl_interface wl_display_interface;

const wl_interface *WlDisplay::Interface() const
{
	return &wl_display_interface;
}

int WlDisplay::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleSync(args[0].n);
			return 0;
		case 1:
			HandleGetRegistry(args[0].n);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_registry_interface;

const wl_interface *WlRegistry::Interface() const
{
	return &wl_registry_interface;
}

int WlRegistry::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleBind(args[0].u, args[1].s, args[2].u, args[3].n);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_callback_interface;

const wl_interface *WlCallback::Interface() const
{
	return &wl_callback_interface;
}

int WlCallback::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	return -1;
}


extern const struct wl_interface wl_compositor_interface;

const wl_interface *WlCompositor::Interface() const
{
	return &wl_compositor_interface;
}

int WlCompositor::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleCreateSurface(args[0].n);
			return 0;
		case 1:
			HandleCreateRegion(args[0].n);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_shm_pool_interface;

const wl_interface *WlShmPool::Interface() const
{
	return &wl_shm_pool_interface;
}

int WlShmPool::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleCreateBuffer(args[0].n, args[1].i, args[2].i, args[3].i, args[4].i, args[5].u);
			return 0;
		case 1:
			HandleDestroy();
			return 0;
		case 2:
			HandleResize(args[0].i);
			return 0;
	}
	return -1;
}

void WlShmPool::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_shm_interface;

const wl_interface *WlShm::Interface() const
{
	return &wl_shm_interface;
}

int WlShm::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleCreatePool(args[0].n, args[1].h, args[2].i);
			return 0;
		case 1:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlShm::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_buffer_interface;

const wl_interface *WlBuffer::Interface() const
{
	return &wl_buffer_interface;
}

int WlBuffer::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
	}
	return -1;
}

void WlBuffer::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_data_offer_interface;

const wl_interface *WlDataOffer::Interface() const
{
	return &wl_data_offer_interface;
}

int WlDataOffer::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleAccept(args[0].u, args[1].s);
			return 0;
		case 1:
			HandleReceive(args[0].s, args[1].h);
			return 0;
		case 2:
			HandleDestroy();
			return 0;
		case 3:
			HandleFinish();
			return 0;
		case 4:
			HandleSetActions(args[0].u, args[1].u);
			return 0;
	}
	return -1;
}

void WlDataOffer::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_data_source_interface;

const wl_interface *WlDataSource::Interface() const
{
	return &wl_data_source_interface;
}

int WlDataSource::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleOffer(args[0].s);
			return 0;
		case 1:
			HandleDestroy();
			return 0;
		case 2:
			HandleSetActions(args[0].u);
			return 0;
	}
	return -1;
}

void WlDataSource::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_data_device_interface;

const wl_interface *WlDataDevice::Interface() const
{
	return &wl_data_device_interface;
}

int WlDataDevice::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleStartDrag((wl_resource*)args[0].o, (wl_resource*)args[1].o, (wl_resource*)args[2].o, args[3].u);
			return 0;
		case 1:
			HandleSetSelection((wl_resource*)args[0].o, args[1].u);
			return 0;
		case 2:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlDataDevice::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_data_device_manager_interface;

const wl_interface *WlDataDeviceManager::Interface() const
{
	return &wl_data_device_manager_interface;
}

int WlDataDeviceManager::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleCreateDataSource(args[0].n);
			return 0;
		case 1:
			HandleGetDataDevice(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_shell_interface;

const wl_interface *WlShell::Interface() const
{
	return &wl_shell_interface;
}

int WlShell::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleGetShellSurface(args[0].n, (wl_resource*)args[1].o);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_shell_surface_interface;

const wl_interface *WlShellSurface::Interface() const
{
	return &wl_shell_surface_interface;
}

int WlShellSurface::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandlePong(args[0].u);
			return 0;
		case 1:
			HandleMove((wl_resource*)args[0].o, args[1].u);
			return 0;
		case 2:
			HandleResize((wl_resource*)args[0].o, args[1].u, args[2].u);
			return 0;
		case 3:
			HandleSetToplevel();
			return 0;
		case 4:
			HandleSetTransient((wl_resource*)args[0].o, args[1].i, args[2].i, args[3].u);
			return 0;
		case 5:
			HandleSetFullscreen(args[0].u, args[1].u, (wl_resource*)args[2].o);
			return 0;
		case 6:
			HandleSetPopup((wl_resource*)args[0].o, args[1].u, (wl_resource*)args[2].o, args[3].i, args[4].i, args[5].u);
			return 0;
		case 7:
			HandleSetMaximized((wl_resource*)args[0].o);
			return 0;
		case 8:
			HandleSetTitle(args[0].s);
			return 0;
		case 9:
			HandleSetClass(args[0].s);
			return 0;
	}
	return -1;
}


extern const struct wl_interface wl_surface_interface;

const wl_interface *WlSurface::Interface() const
{
	return &wl_surface_interface;
}

int WlSurface::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleAttach((wl_resource*)args[0].o, args[1].i, args[2].i);
			return 0;
		case 2:
			HandleDamage(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 3:
			HandleFrame(args[0].n);
			return 0;
		case 4:
			HandleSetOpaqueRegion((wl_resource*)args[0].o);
			return 0;
		case 5:
			HandleSetInputRegion((wl_resource*)args[0].o);
			return 0;
		case 6:
			HandleCommit();
			return 0;
		case 7:
			HandleSetBufferTransform(args[0].i);
			return 0;
		case 8:
			HandleSetBufferScale(args[0].i);
			return 0;
		case 9:
			HandleDamageBuffer(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 10:
			HandleOffset(args[0].i, args[1].i);
			return 0;
	}
	return -1;
}

void WlSurface::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_seat_interface;

const wl_interface *WlSeat::Interface() const
{
	return &wl_seat_interface;
}

int WlSeat::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleGetPointer(args[0].n);
			return 0;
		case 1:
			HandleGetKeyboard(args[0].n);
			return 0;
		case 2:
			HandleGetTouch(args[0].n);
			return 0;
		case 3:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlSeat::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_pointer_interface;

const wl_interface *WlPointer::Interface() const
{
	return &wl_pointer_interface;
}

int WlPointer::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleSetCursor(args[0].u, (wl_resource*)args[1].o, args[2].i, args[3].i);
			return 0;
		case 1:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlPointer::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_keyboard_interface;

const wl_interface *WlKeyboard::Interface() const
{
	return &wl_keyboard_interface;
}

int WlKeyboard::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlKeyboard::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_touch_interface;

const wl_interface *WlTouch::Interface() const
{
	return &wl_touch_interface;
}

int WlTouch::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlTouch::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_output_interface;

const wl_interface *WlOutput::Interface() const
{
	return &wl_output_interface;
}

int WlOutput::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleRelease();
			return 0;
	}
	return -1;
}

void WlOutput::HandleRelease()
{
	Destroy();
}


extern const struct wl_interface wl_region_interface;

const wl_interface *WlRegion::Interface() const
{
	return &wl_region_interface;
}

int WlRegion::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleAdd(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
		case 2:
			HandleSubtract(args[0].i, args[1].i, args[2].i, args[3].i);
			return 0;
	}
	return -1;
}

void WlRegion::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_subcompositor_interface;

const wl_interface *WlSubcompositor::Interface() const
{
	return &wl_subcompositor_interface;
}

int WlSubcompositor::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleGetSubsurface(args[0].n, (wl_resource*)args[1].o, (wl_resource*)args[2].o);
			return 0;
	}
	return -1;
}

void WlSubcompositor::HandleDestroy()
{
	Destroy();
}


extern const struct wl_interface wl_subsurface_interface;

const wl_interface *WlSubsurface::Interface() const
{
	return &wl_subsurface_interface;
}

int WlSubsurface::Dispatch(uint32_t opcode, const struct wl_message *message, union wl_argument *args)
{
	switch (opcode) {
		case 0:
			HandleDestroy();
			return 0;
		case 1:
			HandleSetPosition(args[0].i, args[1].i);
			return 0;
		case 2:
			HandlePlaceAbove((wl_resource*)args[0].o);
			return 0;
		case 3:
			HandlePlaceBelow((wl_resource*)args[0].o);
			return 0;
		case 4:
			HandleSetSync();
			return 0;
		case 5:
			HandleSetDesync();
			return 0;
	}
	return -1;
}

void WlSubsurface::HandleDestroy()
{
	Destroy();
}
