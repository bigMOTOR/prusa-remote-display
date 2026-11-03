// SPDX-FileCopyrightText: 2026 Mykola Fiantsev
// SPDX-License-Identifier: GPL-3.0-only

#include "surface.hpp"

namespace prd::provisioning {

std::string_view log_label(const Surface surface) {
    switch (surface) {
    case Surface::Unknown:
        return "unknown";

    case Surface::SetupAccessPoint:
        return "setup-ap";

    case Surface::StationLan:
        return "station-lan";
    }

    return "unknown";
}

bool permits_mutation(const Surface surface) {
    return surface == Surface::SetupAccessPoint;
}

bool origin_matches_host(const std::string_view origin, const std::string_view host) {
    if (host.empty()) {
        return false;
    }

    // Length before content. A caller that appended anything — a port, a path, another
    // label — is rejected here rather than by a comparison that might stop early.
    if (origin.size() != kOriginScheme.size() + host.size()) {
        return false;
    }

    return origin.compare(0, kOriginScheme.size(), kOriginScheme) == 0 &&
           origin.compare(kOriginScheme.size(), host.size(), host) == 0;
}

std::string setup_url_for(const std::string_view access_point) {
    if (access_point.empty()) {
        return {};
    }

    return std::string{kOriginScheme} + std::string{access_point};
}

void Surfaces::set_access_point(const std::string_view host) {
    access_point_.assign(host);
}

void Surfaces::set_station(const std::string_view host) {
    station_.assign(host);
}

void Surfaces::set_hostname(const std::string_view host) {
    hostname_.assign(host);
}

void Surfaces::clear() {
    access_point_.clear();
    station_.clear();
    hostname_.clear();
}

bool Surfaces::allows_origin(const std::string_view origin) const {
    if (origin.empty()) {
        return false;
    }

    return origin_matches_host(origin, access_point_.view()) ||
           origin_matches_host(origin, station_.view()) ||
           origin_matches_host(origin, hostname_.view());
}

Surface Surfaces::classify(const std::string_view local_address) const {
    if (local_address.empty()) {
        return Surface::Unknown;
    }

    if (!access_point_.empty() && local_address == access_point_.view()) {
        return Surface::SetupAccessPoint;
    }

    // Any other address this device answered on is the user's network. Deliberately not
    // conditional on the station slot being populated: the connection was accepted on a real
    // local address that is not the access point's, and a station address the application
    // task has not published yet does not make that connection privileged.
    return Surface::StationLan;
}

} // namespace prd::provisioning
