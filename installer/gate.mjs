// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only
export const BOARD = "waveshare_esp32_s3_lcd_2_8";
export function validRelease(value) {
  return value?.available === true && value.board === BOARD &&
    /^v\d+\.\d+\.\d+$/.test(value.tag) && /^[a-f0-9]{40}$/.test(value.commit) &&
    /^[a-f0-9]{64}$/.test(value.imageSha256) &&
    /^[a-f0-9]{64}$/.test(value.manifestSha256) &&
    /^[a-f0-9]{64}$/.test(value.publisherSpkiSha256) &&
    value.manifest === "firmware/web-manifest.json";
}
export function mayInstall(release, boardConfirmed, eraseConfirmed, serial, secure) {
  return validRelease(release) && boardConfirmed === true && eraseConfirmed === true &&
    serial === true && secure === true;
}
