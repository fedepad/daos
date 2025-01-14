//
// (C) Copyright 2021 Intel Corporation.
//
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

package storage

import (
	"context"
	"fmt"
	"os"
	"testing"

	"github.com/google/go-cmp/cmp"
	"github.com/google/go-cmp/cmp/cmpopts"
	"github.com/pkg/errors"

	"github.com/daos-stack/daos/src/control/common"
	"github.com/daos-stack/daos/src/control/logging"
)

// defBdevCmpOpts returns a default set of cmp option suitable for this package
func defBdevCmpOpts() []cmp.Option {
	return []cmp.Option{
		// ignore these fields on most tests, as they are intentionally not stable
		cmpopts.IgnoreFields(NvmeController{}, "HealthStats", "Serial"),
	}
}

func Test_scanBdevs(t *testing.T) {
	for name, tc := range map[string]struct {
		scanReq  BdevScanRequest
		cache    *BdevScanResponse
		scanResp *BdevScanResponse
		scanErr  error
		expMsg   string
		expResp  *BdevScanResponse
		expErr   error
	}{
		"bypass cache": {
			scanReq: BdevScanRequest{BypassCache: true},
			cache: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
			scanResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(2),
			},
			expResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(2),
			},
		},
		"bypass cache; scan error": {
			scanReq: BdevScanRequest{BypassCache: true},
			cache: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
			scanResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(2),
			},
			scanErr: errors.New("fail"),
			expErr:  errors.New("fail"),
		},
		"ignore nil cache": {
			scanReq: BdevScanRequest{},
			scanResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
			expResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
		},
		"ignore empty cache": {
			scanReq: BdevScanRequest{},
			cache:   &BdevScanResponse{},
			scanResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
			expResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
		},
		"ignore nil cache; no devices in scan": {
			scanReq: BdevScanRequest{},
			scanResp: &BdevScanResponse{
				Controllers: NvmeControllers{},
			},
			expResp: &BdevScanResponse{
				Controllers: NvmeControllers{},
			},
		},
		"use cache": {
			scanReq: BdevScanRequest{},
			cache: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
			scanResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(2),
			},
			expResp: &BdevScanResponse{
				Controllers: MockNvmeControllers(3),
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(name)
			defer common.ShowBufferOnFailure(t, buf)

			scanFn := func(r BdevScanRequest) (*BdevScanResponse, error) {
				return tc.scanResp, tc.scanErr
			}

			gotResp, gotErr := scanBdevs(log, tc.scanReq, tc.cache, scanFn)
			common.CmpErr(t, tc.expErr, gotErr)
			if gotErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expResp, gotResp, defBdevCmpOpts()...); diff != "" {
				t.Fatalf("\nunexpected response (-want, +got):\n%s\n", diff)
			}
		})
	}
}

func Test_BdevWriteRequestFromConfig(t *testing.T) {
	hostname, err := os.Hostname()
	if err != nil {
		t.Fatal(err)
	}

	for name, tc := range map[string]struct {
		cfg       *Config
		getTopoFn topologyGetter
		expReq    BdevWriteConfigRequest
		expErr    error
	}{
		"nil config": {
			expErr: errors.New("nil config"),
		},
		"nil topo function": {
			cfg:    &Config{},
			expErr: errors.New("nil GetTopology"),
		},
		"no bdev configs": {
			cfg: &Config{
				Tiers: TierConfigs{
					NewTierConfig().
						WithStorageClass(ClassDcpm.String()).
						WithScmMountPoint(fmt.Sprintf("/mnt/daos0")).
						WithScmDeviceList(fmt.Sprintf("/dev/pmem0")),
				},
				EnableHotplug: true,
			},
			getTopoFn: MockGetTopology,
			expReq: BdevWriteConfigRequest{
				OwnerUID:       os.Geteuid(),
				OwnerGID:       os.Getegid(),
				TierProps:      []BdevTierProperties{},
				Hostname:       hostname,
				HotplugEnabled: true,
			},
		},
		"hotplug disabled": {
			cfg: &Config{
				Tiers: TierConfigs{
					NewTierConfig().
						WithStorageClass(ClassDcpm.String()).
						WithScmMountPoint(fmt.Sprintf("/mnt/daos0")).
						WithScmDeviceList(fmt.Sprintf("/dev/pmem0")),
					NewTierConfig().
						WithStorageClass(ClassNvme.String()).
						WithBdevBusidRange("0x00-0x7f"),
				},
			},
			getTopoFn: MockGetTopology,
			expReq: BdevWriteConfigRequest{
				OwnerUID: os.Geteuid(),
				OwnerGID: os.Getegid(),
				TierProps: []BdevTierProperties{
					{Class: ClassNvme},
				},
				Hostname: hostname,
			},
		},
		"range specified": {
			cfg: &Config{
				Tiers: TierConfigs{
					NewTierConfig().
						WithStorageClass(ClassDcpm.String()).
						WithScmMountPoint(fmt.Sprintf("/mnt/daos0")).
						WithScmDeviceList(fmt.Sprintf("/dev/pmem0")),
					NewTierConfig().
						WithStorageClass(ClassNvme.String()).
						WithBdevBusidRange("0x70-0x7f"),
				},
				EnableHotplug: true,
			},
			getTopoFn: MockGetTopology,
			expReq: BdevWriteConfigRequest{
				OwnerUID: os.Geteuid(),
				OwnerGID: os.Getegid(),
				TierProps: []BdevTierProperties{
					{Class: ClassNvme},
				},
				Hostname:          hostname,
				HotplugEnabled:    true,
				HotplugBusidBegin: 0x70,
				HotplugBusidEnd:   0x7f,
			},
		},
		"range unspecified": {
			cfg: &Config{
				Tiers: TierConfigs{
					NewTierConfig().
						WithStorageClass(ClassDcpm.String()).
						WithScmMountPoint(fmt.Sprintf("/mnt/daos0")).
						WithScmDeviceList(fmt.Sprintf("/dev/pmem0")),
					NewTierConfig().
						WithStorageClass(ClassNvme.String()),
				},
				EnableHotplug: true,
			},
			getTopoFn: MockGetTopology,
			expReq: BdevWriteConfigRequest{
				OwnerUID: os.Geteuid(),
				OwnerGID: os.Getegid(),
				TierProps: []BdevTierProperties{
					{Class: ClassNvme},
				},
				Hostname:        hostname,
				HotplugEnabled:  true,
				HotplugBusidEnd: 0x07,
			},
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(name)
			defer common.ShowBufferOnFailure(t, buf)

			gotReq, gotErr := BdevWriteConfigRequestFromConfig(context.TODO(), log, tc.cfg,
				tc.getTopoFn)
			common.CmpErr(t, tc.expErr, gotErr)
			if gotErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expReq, gotReq, defBdevCmpOpts()...); diff != "" {
				t.Fatalf("\nunexpected generated request (-want, +got):\n%s\n", diff)
			}
		})
	}
}
