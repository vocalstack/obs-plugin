#include "net.hpp"

Net &Net::instance()
{
	static Net net;
	return net;
}

Net::Net()
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	multi = curl_multi_init();
	// A few calls at a time, each answering within seconds: a short poll
	// costs nothing next to OBS's own work, and only runs while one is out.
	timer.setInterval(15);
	connect(&timer, &QTimer::timeout, this, &Net::tick);
}

Net::~Net()
{
	for (auto it = running.begin(); it != running.end(); ++it)
		curl_multi_remove_handle(multi, it.key());
	curl_multi_cleanup(multi);
}

void Net::add(CURL *easy, Done done, bool keepAttached)
{
	running.insert(easy, {std::move(done), keepAttached});
	curl_multi_add_handle(multi, easy);
	if (!timer.isActive())
		timer.start();
}

void Net::remove(CURL *easy)
{
	running.remove(easy);
	curl_multi_remove_handle(multi, easy);
}

void Net::tick()
{
	int active = 0;
	curl_multi_perform(multi, &active);
	int left = 0;
	while (CURLMsg *msg = curl_multi_info_read(multi, &left)) {
		if (msg->msg != CURLMSG_DONE)
			continue;
		CURL *easy = msg->easy_handle;
		const CURLcode result = msg->data.result;
		const Transfer t = running.take(easy);
		if (!t.keepAttached || result != CURLE_OK)
			curl_multi_remove_handle(multi, easy);
		if (t.done)
			t.done(result);
	}
	if (running.isEmpty())
		timer.stop();
}
