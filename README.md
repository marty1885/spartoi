# spartoi

Highly concurrent and multi-threaded Spartan Protocol library for the Drogon web application framework.

Spartoi is basically a heavly modified version of [Dremini](https://github.com/marty1885/dremini) to work with Spartan

**This library is in early development. API may clange without notice**


## Dependencies

* [Drogon](https://github.com/drogonframework/drogon) >= 1.7.3 (or 1.7.2 with MIME patch)

## Usage

### Client

Async-callback API

There's several ways to use the Gemini client. The most general one is by the async-callback API. The supplied callback function will be invoked once the cliend recived a response from the server.

```c++
spartoi::sendRequest("spartan://mozz.us/"
    , [](ReqResult result, const HttpResponsePtr& resp) {
        if(result != ReqResult::Ok)
        {
            LOG_ERROR << "Failed to send request";
            return;
        }

        LOG_INFO << "Body size: " << resp->body().size();
    });
```

C++ Coroutines

If C++ coroutines (requires MSVC 19.25, GCC 11 or better) is avaliable to you. You can also use the more stright forward coroutine API.

```c++
try
{
    auto resp = co_await spartoi::sendRequestCoro("spartan://mozz.us/");
    LOG_INFO << "Body size: " << resp->body().size();
} 
catch(...)
{
    LOG_ERROR << "Failed to send request";
}
```

### Server

The `spartoi::SpartanServer` plugin that parses and forwards Spartan requests as HTTP Get requests.

Thus, first enable the Gemini server plugin. And tell the server to serve gemini files (if you are also using it as a file server):

```json
// config.json
{
    "app":{
        "document_root": "./",
        "file_types": [
            "gmi"
        ],
        "use_implicit_page": true,
        "implicit_page": "index.gmi",
        "home_page": "index.gmi",
        "mime": {
            "text/gemini": ["gmi", "gemini"]
        }
    },
    "plugins": [
        {
            "name": "spartoi::SpartanServer",
            "config": {
                "listeners": [
                    {
                        "ip": "127.0.0.1",
						"port": 3000
                    }
                ],
                "numThread": 3
            }
        }
    ]
}

```

Then let's code up ca basic request handler:

```c++
app().registerHandler("/hi", [](const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback)
{
    auto resp = HttpResponse::newHttpResponse();
    resp->setBody("# Hello Gemini\nHello!\n");
    resp->setContentTypeCodeAndCustomString(CT_CUSTOM, "text/gemini");
    callback(resp);
});
```

Finally, open Lagrange and enter the url `spartan://127.0.0.1/hi`. And you'll see the hello message


## Mapping from Gemini to HTTP

Drogon is a HTTP framework. Dremini reuses a large portion of it's original functionality to provide easy Spartan server programming.
We try hard to keep to Spartan sementic in HTTP.

### Status codes

Spartan has a VERY small list of status codes. Namely, Ok, Server Error, Client Error and Redirect. The following is the conversion table
|HTTP|Spartan|
|----|-------|
|2xx |2      |
|307 |3      |
|308 |3      |
|400 |4      |

If the returned HTTP status is not listed above. The Spartan status is simply 5 HttpResponses from a Spartan client automaticallyi have a `spartan-status` header that stores the Spartan status code before mapping.

Spartoi also includes a Gemini compatiablity hack. If the status code is 10 or 11, indicating a Gemini 10 USER INPUT response. It generates a custom form for the user to enter data.

### Serving Gemini files

Drogon by design is a HTTP server. Thus is has no idea about Gemini files and convension. The following config tells Drogon the following things. Which are required to properly serve gemini pages

* Sets the page root folder to `/path/to/the/files`
* Serve `*.gmi` files
    * Add more types if needed
* `*.gmi` and `*.gemini` files have the MIME type `text/gemini`
* When asked to serve a directory, Try serving `index.gmi`
* The homepage is `/index.gmi`

```json
{
    "app":{
        "document_root": "/path/to/the/files",
        "file_types": [
            "gmi",
            "gemini"
        ],
        "use_implicit_page": true,
        "implicit_page": "index.gmi",
        "home_page": "index.gmi",
        "mime": {
            "text/gemini": ["gmi", "gemini"]
        }
    }
}
```

### Gemini-style query

Gemini URLs supports a query parameter using the `?` symbol. For example, `gemini://localhost/search?Hello` has a query of "Hello". Dremini adds a `query` parameter to the HttpRequest when a query is detected.

### Detecting Spartan requests

Spartoi adds a `protocol` header to the proxyed HTTP request to singnal it's comming from a Gemini request. Whom's value is always "spartoi"

