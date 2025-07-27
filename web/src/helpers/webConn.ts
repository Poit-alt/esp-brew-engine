// Wrapper function for webrequests so we can change the used class or total backend logic
import type { IApiResult } from "@/interfaces/IApiResult";

export default class WebConn {
  public rootUrl: string | null = null;

  constructor(rootUrl: string) {
    this.rootUrl = rootUrl;
  }

  doPostRequest(data: any): Promise<IApiResult> {
    return new Promise((resolve, reject) => {
      const url = `${this.rootUrl}api`;

      const response = fetch(url, {
        method: "POST", // *GET, POST, PUT, DELETE, etc.
        mode: "cors", // no-cors, *cors, same-origin //no-cors doesn't give any data, only gives error about json parse (bug?)
        cache: "no-cache", // *default, no-cache, reload, force-cache, only-if-cached
        credentials: "omit", // include, *same-origin, omit
        headers: {
          "Content-Type": "application/json",
        },
        // Add timeout for Access Point mode compatibility
        signal: AbortSignal.timeout(5000),
        // redirect: "follow", // manual, *follow, error
        // referrerPolicy: "no-referrer", // no-referrer, *no-referrer-when-downgrade, origin, origin-when-cross-origin, same-origin, strict-origin, strict-origin-when-cross-origin, unsafe-url
        body: JSON.stringify(data), // body data type must match "Content-Type" header
      })
        .then((result) => {
          if (!result.ok) {
            throw new Error(`HTTP ${result.status}: ${result.statusText}`);
          }
          const apiResult = result.json();
          resolve(apiResult);
        })
        .catch((error) => {
          console.warn("API request failed:", error.message || error);
          const apiResult: IApiResult = {
            success: false,
            data: null,
            message: error.message || error.toString(),
          };
          // Resolve with failed result instead of rejecting to prevent unhandled promise rejections
          resolve(apiResult);
        });
    });
  }
}
