//
// Created by yangbin on 2021/3/28.
//

#include "data_source.h"

#include <logging.h>

namespace media {

DataSourceHost::~DataSourceHost() = default;

void DataSource::SetHost(DataSourceHost *host) {
  DCHECK(host);
  DCHECK(!host_);
  host_ = host;
}

DataSourceHost *DataSource::host() {
  return host_;
}

DataSource::~DataSource() = default;

}
