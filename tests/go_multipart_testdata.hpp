// Go 1.27.3's mime/multipart/testdata/nested-mime: the body of a multipart/mixed
// email with boundary e89a8ff1c1e83553e304be640612.
#pragma once

#include <string_view>

inline constexpr std::string_view nestedMime =
    "--e89a8ff1c1e83553e304be640612\r\n"
    "Content-Type: multipart/alternative; boundary=e89a8ff1c1e83553e004be640610\r\n"
    "\r\n"
    "--e89a8ff1c1e83553e004be640610\r\n"
    "Content-Type: text/plain; charset=UTF-8\r\n"
    "\r\n"
    "*body*\r\n"
    "\r\n"
    "--e89a8ff1c1e83553e004be640610\r\n"
    "Content-Type: text/html; charset=UTF-8\r\n"
    "\r\n"
    "<b>body</b>\r\n"
    "\r\n"
    "--e89a8ff1c1e83553e004be640610--\r\n"
    "--e89a8ff1c1e83553e304be640612\r\n"
    "Content-Type: image/png; name=\"x.png\"\r\n"
    "Content-Disposition: attachment; \r\n"
    "\011filename=\"x.png\"\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "X-Attachment-Id: f_h1edgigu0\r\n"
    "\r\n"
    "iVBORw0KGgoAAAANSUhEUgAAAagAAADrCAIAAACza5XhAAAKMWlDQ1BJQ0MgUHJvZmlsZQAASImd\r\n"
    "lndUU9kWh8+9N71QkhCKlNBraFICSA29SJEuKjEJEErAkAAiNkRUcERRkaYIMijggKNDkbEiioUB\r\n"
    "8b2kqeGaj4aTNftesu5mob4pr07ecMywRwLBvDCJOksqlUyldAZD7g9fxIZRWWPMvXRNJROJRBIG\r\n"
    "Y7Vx0mva1HAwYqibdKONXye3dW4iUonhWFJnqK7OaanU1gGkErFYEgaj0cg8wK+zVPh2ziwnHy07\r\n"
    "U8lYTNapezSzOuevRwLB7CFkqQQCwaJDiBQIBIJFhwh8AoFg0SHUqQUCASRJKkwkhMy/JfODWPEJ\r\n"
    "BIJFhwh8AoFg0TFnQqQ55GtPFopcJsN97e1nYtNuIBYeGBgYCmYrmE3jZ05iaGAoMX0xzxkWz6Hv\r\n"
    "yO7WvrlwzA0uLzrD+VkKqViwl9IfTBVNFMyc/x9alloiPPlqhQAAAABJRU5ErkJggg==\r\n"
    "--e89a8ff1c1e83553e304be640612--\r\n";
