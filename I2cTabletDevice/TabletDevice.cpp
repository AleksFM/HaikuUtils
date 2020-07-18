#include "TabletDevice.h"

#include <stdio.h>
#include <string.h>

enum {
	kTabletThreadPriority = B_FIRST_REAL_TIME_PRIORITY + 4,
};

enum {
	second = 1000000,
	doubleClickDelay = second / 10, // TODO: read from settings
	snoozeDelay = second / 100,
};

template<typename Type>
inline static void SetBit(Type &val, int bit) {val |= Type(1) << bit;}
template<typename Type>
inline static void ClearBit(Type &val, int bit) {val &= ~(Type(1) << bit);}
template<typename Type>
inline static void InvertBit(Type &val, int bit) {val ^= Type(1) << bit;}
template<typename Type>
inline static void SetBitTo(Type &val, int bit, bool isSet) {val ^= ((isSet? -1: 0) ^ val) & (Type(1) << bit);}
template<typename Type>
inline static bool IsBitSet(Type val, int bit) {return (val & (Type(1) << bit)) != 0;}

int I2cIo(
	int fd, i2c_op op, i2c_addr addr,
	const void *cmd, size_t cmdLen, void* data,
	size_t dataLen
)
{
	i2c_ioctl_exec exec;
	exec.addr = addr;
	exec.op = op;
	exec.cmdBuffer = cmd;
	exec.cmdLength = cmdLen;
	exec.buffer = data;
	exec.bufferLength = dataLen;

	return ioctl(fd, I2CEXEC, &exec, sizeof(exec));
}

int I2cRead(
	int fd, i2c_addr addr,
	const void *cmd, size_t cmdLen, void* data,
	size_t dataLen
)
{
	return I2cIo(fd, I2C_OP_READ_STOP, addr, cmd, cmdLen, data, dataLen);
}

int I2cWrite(
	int fd, i2c_addr addr,
	const void *cmd, size_t cmdLen, void* data,
	size_t dataLen
)
{
	return I2cIo(fd, I2C_OP_WRITE_STOP, addr, cmd, cmdLen, data, dataLen);
}


void SetTabletState(TabletState &s, TabletPacket &p)
{
	s.buttons = 0;
	s.pressure = 0;
	if (inRangeFlag & p.flags) {
		s.when = system_time();
		s.x = float(p.x) / xMax;
		s.y = float(p.y) / yMax;
		s.pressure = float(p.pressure) / pressureMax;
		if (tipSwitchFlag & p.flags)
			s.buttons |= 1 << 0;
		if ((barrelSwitchFlag & p.flags) || (eraserFlag & p.flags))
			s.buttons |= 1 << 1;
	}
}

bool FillMessage(BMessage &msg, const TabletState &s)
{
	if (msg.AddInt64("when", s.when) < B_OK
		|| msg.AddInt32("buttons", s.buttons) < B_OK
		|| msg.AddFloat("x", s.x) < B_OK
		|| msg.AddFloat("y", s.y) < B_OK) {
		return false;
	}
	msg.AddFloat("be:tablet_x", s.x);
	msg.AddFloat("be:tablet_y", s.y);
	msg.AddFloat("be:tablet_pressure", s.pressure);
	return true;
}

TabletDevice::TabletDevice():
	fLastClick(-1),
	fLastClickBtn(-1),
	fWatcherThread(-1),
	fRun(false)
{
}

TabletDevice::~TabletDevice()
{
}


status_t TabletDevice::InitCheck()
{
	static input_device_ref tablet = {(char*)"I2C Tablet", B_KEYBOARD_DEVICE, (void*)this};
	static input_device_ref *devices[2] = {&tablet, NULL};

	fDeviceAdr = 0x9;
	FileDescriptorCloser fd(open("/dev/bus/i2c/1/bus_raw", O_RDWR));
	if (fd.Get() < 0)
		return B_ERROR;

	uint16 cmd;

	cmd = 1;
	if (I2cRead(fd.Get(), fDeviceAdr, &cmd, sizeof(cmd), &fDesc, sizeof(fDesc)) != 0)
		return B_ERROR;

	if (!(
		fDesc.wHIDDescLength == 0x1e &&
		fDesc.wVendorID == 0x2d1f &&
		fDesc.wProductID == 0x34 &&
		fDesc.wVersionID == 0x1321
	))
		return B_ERROR;

	fDeviceFd.SetTo(fd.Detach());

	RegisterDevices(devices);
	return B_OK;
}


status_t TabletDevice::Start(const char* name, void* cookie)
{
	char threadName[B_OS_NAME_LENGTH];
	snprintf(threadName, B_OS_NAME_LENGTH, "%s watcher", name);

	if (fWatcherThread < 0) {
		fWatcherThread = spawn_thread(DeviceWatcher, threadName, kTabletThreadPriority, this);

		if (fWatcherThread < B_OK)
			return fWatcherThread;

		fRun = true;
		resume_thread(fWatcherThread);
	}
	return B_OK;
}

status_t TabletDevice::Stop(const char* name, void* cookie)
{
	if (fWatcherThread >= B_OK) {
		suspend_thread(fWatcherThread);
		fRun = false;
		status_t res;
		wait_for_thread(fWatcherThread, &res);
		fWatcherThread = -1;
	}
	return B_OK;
}

status_t TabletDevice::Control(const char* name, void* cookie, uint32 command, BMessage* message)
{
	return B_OK;
}

int32 TabletDevice::DeviceWatcher(void *arg)
{
	TabletDevice &t = *((TabletDevice*)arg);

	while (t.fRun) {
		bigtime_t when;
		uint16 cmd;
		TabletPacket packet;
		TabletState state;

		cmd = t.fDesc.wInputRegister;
		if (
			I2cRead(t.fDeviceFd.Get(), t.fDeviceAdr, &cmd, sizeof(cmd), &packet, sizeof(packet)) == 0 &&
			packet.size == 0x0a &&
			packet.reportId == 2
		) {
			SetTabletState(state, packet);
			t.fState.when = state.when;

			// update pos
			if (t.fState.x != state.x || t.fState.y != state.y || t.fState.pressure != state.pressure) {
				t.fState.x = state.x;
				t.fState.y = state.y;
				t.fState.pressure = state.pressure;
				ObjectDeleter<BMessage> msg(new BMessage(B_MOUSE_MOVED));
				if (msg.Get() == NULL) continue;
				if (!FillMessage(*msg.Get(), t.fState)) continue;
				t.EnqueueMessage(msg.Detach());
			}

			// update buttons
			for (int i = 0; i < 32; i++) {
				if ((IsBitSet(t.fState.buttons, i) != IsBitSet(state.buttons, i))) {
					InvertBit(t.fState.buttons, i);
					ObjectDeleter<BMessage> msg(new BMessage());
					if (msg.Get() == NULL) continue;
					if (!FillMessage(*msg.Get(), t.fState)) continue;
					if (IsBitSet(t.fState.buttons, i)) {
						msg->what = B_MOUSE_DOWN;
						if (i == t.fLastClickBtn && t.fState.when - t.fLastClick >= doubleClickDelay)
							t.fState.clicks++;
						else
							t.fState.clicks = 1;
						t.fLastClickBtn = i;
						t.fLastClick = t.fState.when;
						msg->AddInt32("clicks", t.fState.clicks);
					} else {
						msg->what = B_MOUSE_UP;
					}
					t.EnqueueMessage(msg.Detach());
				}
			}
		}

		snooze(snoozeDelay);
	}
	return B_OK;
}


extern "C" BInputServerDevice *instantiate_input_device()
{
	return new(std::nothrow) TabletDevice();
}
