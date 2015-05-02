#include <node.h>
#include <node_buffer.h>
#include <v8.h>
#include <nan.h>
#include <cstddef>
#include <cassert>
#include <cstring>

#include "blake2.h"

union any_blake2_state {
	blake2b_state casted_blake2b_state;
	blake2bp_state casted_blake2bp_state;
	blake2s_state casted_blake2s_state;
	blake2sp_state casted_blake2sp_state;
};

#define THROW_AND_RETURN_IF_NOT_STRING_OR_BUFFER(val) \
	if (!val->IsString() && !Buffer::HasInstance(val)) { \
		return NanThrowError(Exception::TypeError(NanNew<String>("Not a string or buffer"))); \
	}

#define BLAKE_FN_CAST(fn) \
	reinterpret_cast<int (*)(void *, const unsigned char *, long unsigned int)>(fn)

using namespace node;
using namespace v8;

class Hash: public ObjectWrap {
protected:
	bool initialised_;
	int (*any_blake2_update)(void*, const uint8_t*, uint64_t);
	int (*any_blake2_final)(void*, const uint8_t*, uint64_t);
	int outbytes;
	any_blake2_state state;

public:
	static void
	Initialize(Handle<Object> target) {
		NanScope();
		Local<FunctionTemplate> t = NanNew<FunctionTemplate>(New);
		t->InstanceTemplate()->SetInternalFieldCount(1);
		t->SetClassName(NanNew<String>("Hash"));

		NODE_SET_PROTOTYPE_METHOD(t, "update", Update);
		NODE_SET_PROTOTYPE_METHOD(t, "digest", Digest);

		NanAssignPersistent(constructor, t->GetFunction());
		target->Set(NanNew<String>("Hash"), t->GetFunction());
	}

	static
	NAN_METHOD(New) {
		NanScope();
		Hash *obj;

		if (!args.IsConstructCall()) {
			return NanThrowError("Constructor must be called with new");
		}

		obj = new Hash();
		obj->Wrap(args.This());
		if(args.Length() < 1) {
			return NanThrowError(Exception::TypeError(NanNew<String>("Expected a string argument with algorithm name")));
		}
		if(!args[0]->IsString()) {
			return NanThrowError(Exception::TypeError(NanNew<String>("Algorithm name must be a string")));
		}
		std::string algo = std::string(*String::Utf8Value(args[0]->ToString()));
		if(algo == "blake2b") {
			blake2b_init(reinterpret_cast<blake2b_state*>(&obj->state), BLAKE2B_OUTBYTES);
			obj->outbytes = 512 / 8;
			obj->any_blake2_update = BLAKE_FN_CAST(blake2b_update);
			obj->any_blake2_final = BLAKE_FN_CAST(blake2b_final);
		} else if(algo == "blake2bp") {
			blake2bp_init(reinterpret_cast<blake2bp_state*>(&obj->state), BLAKE2B_OUTBYTES);
			obj->outbytes = 512 / 8;
			obj->any_blake2_update = BLAKE_FN_CAST(blake2bp_update);
			obj->any_blake2_final = BLAKE_FN_CAST(blake2bp_final);
		} else if(algo == "blake2s") {
			blake2s_init(reinterpret_cast<blake2s_state*>(&obj->state), BLAKE2S_OUTBYTES);
			obj->outbytes = 256 / 8;
			obj->any_blake2_update = BLAKE_FN_CAST(blake2s_update);
			obj->any_blake2_final = BLAKE_FN_CAST(blake2s_final);
		} else if(algo == "blake2sp") {
			blake2sp_init(reinterpret_cast<blake2sp_state*>(&obj->state), BLAKE2S_OUTBYTES);
			obj->outbytes = 256 / 8;
			obj->any_blake2_update = BLAKE_FN_CAST(blake2sp_update);
			obj->any_blake2_final = BLAKE_FN_CAST(blake2sp_final);
		} else {
			return NanThrowError("Algorithm must be blake2b, blake2s, blake2bp, or blake2sp");
		}
		obj->initialised_ = true;
		NanReturnValue(args.This());
	}

	static
	NAN_METHOD(Update) {
		NanScope();
		Hash *obj = ObjectWrap::Unwrap<Hash>(args.This());

		THROW_AND_RETURN_IF_NOT_STRING_OR_BUFFER(args[0]);

		if(!obj->initialised_) {
			Local<Value> exception = Exception::Error(NanNew<String>("Not initialized"));
			return NanThrowError(exception);
		}

		enum encoding enc = ParseEncoding(args[1]);
		ssize_t len = DecodeBytes(args[0], enc);

		if (len < 0) {
			Local<Value> exception = Exception::Error(NanNew<String>("Bad argument"));
			return NanThrowError(exception);
		}

		if (Buffer::HasInstance(args[0])) {
			Local<Object> buffer_obj = args[0]->ToObject();
			const char *buffer_data = Buffer::Data(buffer_obj);
			size_t buffer_length = Buffer::Length(buffer_obj);
			obj->any_blake2_update(
				reinterpret_cast<void*>(&obj->state),
				reinterpret_cast<const uint8_t*>(buffer_data),
				buffer_length
			);
		} else {
			char *buf = new char[len];
			ssize_t written = DecodeWrite(buf, len, args[0], enc);
			assert(written == len);
			obj->any_blake2_update(
				reinterpret_cast<void*>(&obj->state),
				reinterpret_cast<const uint8_t*>(buf),
				len
			);
			delete[] buf;
		}

		NanReturnValue(args.This());
	}

	static
	NAN_METHOD(Digest) {
		NanScope();
		v8::Isolate* isolate = v8::Isolate::GetCurrent();
		Hash *obj = ObjectWrap::Unwrap<Hash>(args.This());
		unsigned char digest[512 / 8];

		if(!obj->initialised_) {
			Local<Value> exception = Exception::Error(NanNew<String>("Not initialized"));
			return NanThrowError(exception);
		}

		obj->initialised_ = false;
		obj->any_blake2_final(reinterpret_cast<void*>(&obj->state), digest, obj->outbytes);

		Local<Value> outString;

		enum encoding encoding = BUFFER;
		if (args.Length() >= 1) {
			// TODO: make compatible with pre-iojs nodes
			// https://github.com/iojs/nan/issues/189
			encoding = ParseEncoding(
				isolate,
				args[0]->ToString(isolate),
				BUFFER
			);
		}

		// TODO: make compatible with pre-iojs nodes
		// Need to convert node::Encoding to Nan::Encoding?
		Local<Value> rc = Encode(
			isolate,
			reinterpret_cast<const char*>(digest),
			obj->outbytes,
			encoding
		);

		NanReturnValue(rc);
	}

private:
	static Persistent<Function> constructor;
};

Persistent<Function> Hash::constructor;

static void
init(Handle<Object> target) {
	Hash::Initialize(target);
}

NODE_MODULE(blake2, init)