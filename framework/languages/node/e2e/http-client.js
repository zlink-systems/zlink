"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.getJson = getJson;
exports.getJsonWithin = getJsonWithin;
exports.postJson = postJson;
exports.postJsonWithin = postJsonWithin;
exports.getStatus = getStatus;
exports.postStatus = postStatus;
const http_client_1 = require("@zlink-systems/http-client");
const defaultTimeoutMs = 3_000;
async function getJson(baseUrl, path) {
    const target = requestTarget(baseUrl, path);
    return await http_client_1.ZLinkHttpClient.create(target.origin)
        .timeout(defaultTimeoutMs)
        .get(target.path)
        .fetch();
}
async function getJsonWithin(baseUrl, path, timeoutMs) {
    const target = requestTarget(baseUrl, path);
    return await http_client_1.ZLinkHttpClient.create(target.origin)
        .timeout(timeoutMs)
        .get(target.path)
        .fetch();
}
async function postJson(baseUrl, pathOrBody, body) {
    const hasSeparatePath = typeof pathOrBody === 'string' && pathOrBody.startsWith('/');
    const target = requestTarget(baseUrl, hasSeparatePath ? pathOrBody : undefined);
    const requestBody = hasSeparatePath ? body : pathOrBody;
    const request = http_client_1.ZLinkHttpClient.create(target.origin).timeout(defaultTimeoutMs).post(target.path);
    if (requestBody !== undefined)
        request.body(requestBody);
    return await request.fetch();
}
async function postJsonWithin(baseUrl, path, body, timeoutMs) {
    const target = requestTarget(baseUrl, path);
    return await http_client_1.ZLinkHttpClient.create(target.origin)
        .timeout(timeoutMs)
        .post(target.path)
        .body(body)
        .fetch();
}
async function getStatus(url, timeoutMs = defaultTimeoutMs) {
    return (await rawRequest('GET', url, timeoutMs)).status;
}
async function postStatus(url, timeoutMs = defaultTimeoutMs) {
    return (await rawRequest('POST', url, timeoutMs)).status;
}
async function rawRequest(method, url, timeoutMs) {
    const target = requestTarget(url);
    const client = http_client_1.ZLinkHttpClient.create(target.origin).timeout(timeoutMs);
    return await (method === 'GET' ? client.get(target.path) : client.post(target.path)).submitRaw();
}
function requestTarget(baseUrl, path) {
    const base = new URL(baseUrl);
    const prefix = base.pathname === '/' ? '' : base.pathname.replace(/\/$/, '');
    const suffix = path === undefined ? '' : path.startsWith('/') ? path : `/${path}`;
    const url = path === undefined ? base : new URL(`${prefix}${suffix}`, base.origin);
    return { origin: url.origin, path: `${url.pathname}${url.search}` };
}
