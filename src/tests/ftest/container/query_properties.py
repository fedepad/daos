#!/usr/bin/python3
'''
  (C) Copyright 2018-2022 Intel Corporation.

  SPDX-License-Identifier: BSD-2-Clause-Patent
'''
import ctypes

from apricot import TestWithServers
from pydaos.raw import daos_cref, DaosApiError, conversion, DaosContPropEnum
from test_utils_container import TestContainer


class QueryPropertiesTest(TestWithServers):
    """
    Test Class Description: Verify container properties are set during container query
    over pydaos API.

    :avocado: recursive
    """

    def test_query_properties(self):
        """JIRA ID: DAOS-9515

        Test Description: Verify container properties are correctly set by configuring
        some properties during create.

        Use Cases:
        1. Create a container with some properties related to checksum, type, etc.
        configured.
        2. Call container query. it returns container info such as UUID, snapshots, etc.,
        but not properties. We verify the property by passing in an empty data structure
        as pass by reference.
        3. Verify the property data structure passed in during step 2.

        :avocado: tags=all,full_regression
        :avocado: tags=vm
        :avocado: tags=container
        :avocado: tags=query_properties
        """
        errors = []

        self.add_pool()
        self.container = TestContainer(pool=self.pool)
        self.container.get_params(self)

        # Prepare DaosContProperties. Update some items from default. These are
        # properties that determine the values, not the actual values. The actual values
        # are set in DaosContainer.create() based on these configurations.
        cont_prop_type = bytes("POSIX", "utf-8") # Updated
        enable_chksum = True # Updated
        srv_verify = True # Updated
        chksum_type = ctypes.c_uint64(100) # Default
        chunk_size = ctypes.c_uint64(0) # Default
        con_in = [
            cont_prop_type,
            enable_chksum,
            srv_verify,
            chksum_type,
            chunk_size
        ]

        # Create container with the DaosContProperties.
        self.container.create(con_in=con_in)

        # Open the container.
        self.container.open(pool_handle=self.pool.pool.handle)

        # Prepare the DaosProperty data structure that stores the values that are
        # configured based on the properties we used during create. Here, we create
        # the empty data structure and set the dpe_type fields. The values (dpe_val) will
        # be filled during query. See DaosContainer.create() and DaosContProperties for
        # more details.

        # If DaosContProperties.type is not "Unknown", there will be 4 elements.

        # Element 0: Layout type, which is determined by the container type. If the
        # container type is POSIX, we expect the value to be
        # DaosContPropEnum.DAOS_PROP_CO_LAYOUT_POSIX.value

        # Note: enable_chksum needs to be set to True to get the following 3 elements.
        # Element 1: Checksum. In default, we expect it to be 1.
        # Element 2: Checksum server verify. Since we updated the srv_verify to True, we
        # expect the value to be 1.
        # Element 3: Checksum chunk size. In default we expect it to be 16384.
        daos_property = daos_cref.DaosProperty(4)

        daos_property.dpp_entries[0].dpe_type = ctypes.c_uint32(
            DaosContPropEnum.DAOS_PROP_CO_LAYOUT_TYPE.value)
        daos_property.dpp_entries[1].dpe_type = ctypes.c_uint32(
            DaosContPropEnum.DAOS_PROP_CO_CSUM.value)
        daos_property.dpp_entries[2].dpe_type = ctypes.c_uint32(
            DaosContPropEnum.DAOS_PROP_CO_CSUM_SERVER_VERIFY.value)
        daos_property.dpp_entries[3].dpe_type = ctypes.c_uint32(
            DaosContPropEnum.DAOS_PROP_CO_CSUM_CHUNK_SIZE.value)

        try:
            cont_info = self.container.container.query(daos_property=daos_property)
        except DaosApiError as error:
            self.log.info("Container query error! %s", error)

        # Sanity check that query isn't broken.
        uuid = conversion.c_uuid_to_str(cont_info.ci_uuid)
        self.log.info("UUID obtained from query = %s", uuid)
        if uuid != self.container.container.get_uuid_str():
            msg = ("Container UUID obtained after create and after query don't match! "
                   "Create: {}, Query: {}".format(
                       self.container.container.get_uuid_str(), uuid))
            errors.append(msg)

        # Verify values set in daos_property.
        # Verify layout type.
        layout_type = daos_property.dpp_entries[0].dpe_val
        if layout_type == DaosContPropEnum.DAOS_PROP_CO_LAYOUT_POSIX.value:
            self.log.info("Layout type is POSIX as expected.")
        else:
            self.log.info("Layout type is NOT POSIX!")
            errors.append("Layout type is not POSIX! %s", layout_type)

        # Verify checksum.
        if daos_property.dpp_entries[1].dpe_val != 1:
            msg = "Unexpected checksum from query! Expected = 1; Actual = {}".format(
                daos_property.dpp_entries[1].dpe_val)
            errors.append(msg)

        # Verify server verify.
        if daos_property.dpp_entries[2].dpe_val != 1:
            msg = "Unexpected server verify from query! Expected = 1; Actual = {}".format(
                daos_property.dpp_entries[2].dpe_val)
            errors.append(msg)

        # Verify checksum chunk size.
        if daos_property.dpp_entries[3].dpe_val != 16384:
            msg = ("Unexpected checksum chunk size from query! "
                   "Expected = 16384; Actual = {}".format(
                       daos_property.dpp_entries[3].dpe_val))
            errors.append(msg)

        if errors:
            self.fail("\n----- Errors detected! -----\n{}".format("\n".join(errors)))
