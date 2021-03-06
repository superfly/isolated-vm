#include "session_handle.h"
#include "isolate/util.h"

using namespace v8;
using namespace v8_inspector;
using std::shared_ptr;
using std::unique_ptr;

namespace ivm {

/**
 * This class handles sending messages from the backend to the frontend
 */
class SessionImpl : public InspectorSession {
	public:
		shared_ptr<IsolateHolder> isolate; // This is the isolate that owns the session
		shared_ptr<Persistent<Function>> onNotification;
		shared_ptr<Persistent<Function>> onResponse;

		explicit SessionImpl(IsolateEnvironment& isolate) : InspectorSession(isolate) {}

	private:
		// Helper
		static MaybeLocal<String> bufferToString(StringBuffer& buffer) {
			const StringView& view = buffer.string();
			if (view.is8Bit()) {
				return String::NewFromOneByte(Isolate::GetCurrent(), view.characters8(), v8::NewStringType::kNormal, view.length());
			} else {
				return String::NewFromTwoByte(Isolate::GetCurrent(), view.characters16(), v8::NewStringType::kNormal, view.length());
			}
		}

		// These functions are invoked directly from v8
		void sendResponse(int call_id, unique_ptr<StringBuffer> message) final {
			if (!onResponse) {
				return;
			}
			struct SendResponseTask : public Runnable {
				int call_id;
				unique_ptr<StringBuffer> message;
				shared_ptr<Persistent<Function>> onResponse;

				SendResponseTask(
					int call_id, unique_ptr<StringBuffer> message, shared_ptr<Persistent<Function>> onResponse
				) :	call_id(call_id), message(std::move(message)), onResponse(std::move(onResponse)) {}

				void Run() final {
					Local<String> string;
					if (bufferToString(*message).ToLocal(&string)) {
						Local<Function> fn = Deref(*onResponse);
						Local<Value> argv[2];
						Isolate* isolate = Isolate::GetCurrent();
						argv[0] = Integer::New(isolate, call_id);
						argv[1] = string;
						try {
							Unmaybe(fn->Call(isolate->GetCurrentContext(), Undefined(isolate), 2, argv));
						} catch (const js_runtime_error& err) {}
					}
				}
			};
			isolate->ScheduleTask(std::make_unique<SendResponseTask>(call_id, std::move(message), onResponse), false, true);
		}

		void sendNotification(unique_ptr<StringBuffer> message) final {
			if (!onNotification) {
				return;
			}
			struct SendNotificationTask : public Runnable {
				unique_ptr<StringBuffer> message;
				shared_ptr<Persistent<Function>> onNotification;
				SendNotificationTask(
					unique_ptr<StringBuffer> message, shared_ptr<Persistent<Function>> onNotification
				) : message(std::move(message)), onNotification(std::move(onNotification)) {}

				void Run() final {
					Local<String> string;
					if (bufferToString(*message).ToLocal(&string)) {
						Local<Function> fn = Deref(*onNotification);
						Local<Value> argv[1];
						Isolate* isolate = Isolate::GetCurrent();
						argv[0] = string;
						try {
							Unmaybe(fn->Call(isolate->GetCurrentContext(), Undefined(isolate), 1, argv));
						} catch (const js_runtime_error& err) {}
					}
				}
			};
			isolate->ScheduleTask(std::make_unique<SendNotificationTask>(std::move(message), onNotification), false, true);
		}

		void flushProtocolNotifications() final {}
};

/**
 * SessionHandle implementation
 */
SessionHandle::SessionHandle(IsolateEnvironment& isolate) : session(std::make_shared<SessionImpl>(isolate)) {
	session->isolate = IsolateEnvironment::GetCurrentHolder();
}

IsolateEnvironment::IsolateSpecific<FunctionTemplate>& SessionHandle::TemplateSpecific() {
	static IsolateEnvironment::IsolateSpecific<FunctionTemplate> tmpl;
	return tmpl;
}

Local<FunctionTemplate> SessionHandle::Definition() {
	return MakeClass(
		"Session", nullptr,
		"dispatchProtocolMessage", Parameterize<decltype(&SessionHandle::DispatchProtocolMessage), &SessionHandle::DispatchProtocolMessage>(),
		"dispose", Parameterize<decltype(&SessionHandle::Dispose), &SessionHandle::Dispose>(),
		"onNotification", ParameterizeAccessor<
			decltype(&SessionHandle::OnNotificationGetter), &SessionHandle::OnNotificationGetter,
			decltype(&SessionHandle::OnNotificationSetter), &SessionHandle::OnNotificationSetter
		>(),
		"onResponse", ParameterizeAccessor<
			decltype(&SessionHandle::OnResponseGetter), &SessionHandle::OnResponseGetter,
			decltype(&SessionHandle::OnResponseSetter), &SessionHandle::OnResponseSetter
		>()
	);
}

void SessionHandle::CheckDisposed() {
	if (!session) {
		throw js_generic_error("Session is dead");
	}
}

/**
 * JS API methods
 */
Local<Value> SessionHandle::DispatchProtocolMessage(Local<String> message) {
	CheckDisposed();
	String::Value v8_str(message);
	session->DispatchBackendProtocolMessage(std::vector<uint16_t>(*v8_str, *v8_str + v8_str.length()));
	return Undefined(Isolate::GetCurrent());
}

Local<Value> SessionHandle::Dispose() {
	CheckDisposed();
	session.reset();
	return Undefined(Isolate::GetCurrent());
}

// .onNotification
Local<Value> SessionHandle::OnNotificationGetter() {
	CheckDisposed();
	if (session->onNotification) {
		return Deref(*session->onNotification);
	} else {
		return Undefined(Isolate::GetCurrent());
	}
}

void SessionHandle::OnNotificationSetter(Local<Function> value) {
	CheckDisposed();
	session->onNotification = std::make_shared<Persistent<Function>>(Isolate::GetCurrent(), value);
}

// .onResponse
Local<Value> SessionHandle::OnResponseGetter() {
	CheckDisposed();
	if (session->onResponse) {
		return Deref(*session->onResponse);
	} else {
		return Undefined(Isolate::GetCurrent());
	}
}

void SessionHandle::OnResponseSetter(Local<Function> value) {
	CheckDisposed();
	session->onResponse = std::make_shared<Persistent<Function>>(Isolate::GetCurrent(), value);
}

} // namespace ivm
