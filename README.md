# Radio Service

## Overview

Radio Service allows tuning of rtl-sdr based devices to radio stations and receive
respective audio stream.

## Verbs

| Name               | Description                                 | JSON Parameters                                                |
|:-------------------|:-------------------- -----------------------|:---------------------------------------------------------------|
| subscribe          | subscribe to radio events                   | *Request:* {"value": "frequency"}                              |
| unsubscribe        | unsubscribe to radio events                 | *Request:* {"value": "frequency"}                              |
| frequency          | get/set tuned radio frequency               | *Request:* {"value": 101100000}                                |
| band               | get/set current band type (e.g. AM, FM)     | *Request:* {"band": "FM"} *Response:* {"band": "FM"}           |
| band_supported     | check if a certain band is supported        | *Request:* {"band": "FM"} *Response:* {"supported": 1}         |
| frequency_range    | get frequency range for band type           | *Request:* {"band": "FM"} *Response:* {"min": ..., "max": ...} |
| frequency_step     | get frequency step/spacing for band type    | *Request:* {"band": "FM"} *Response:* {"step": 200000}         |
| start              | start radio playback                        |                                                                |
| stop               | stop radio playback                         |                                                                |
| scan_start         | start scanning for station                  | *Request:* {"direction": "forward" or "backward"}              |
| scan_stop          | stop scanning for station                   |                                                                |
| stereo_mode        | get/set stereo or mono mode                 | *Request:* {"value": "stereo" or "mono"}                       |

## Events

### frequency Event JSON Response

JSON response has a single field **frequency** which is the currently tuned frequency.

### station_found Event JSON Response

JSON response has a single field **value** of the frequency of the discovered radio station.
