// A long-lived Linux host for the BlorgFS Windows test guest, on Azure.
//
// The guest itself always runs under KVM/QEMU (ci/guest/host); this only
// provides somewhere with KVM that stays up, for when a GitHub runner's
// lifetime is too short: iterating on a driver bug, a guest kept at a
// snapshot between sessions, or a self-hosted runner with more cores.
//
// Dsv5 sizes support nested virtualization, which is what exposes /dev/kvm
// inside the VM. Only SSH is reachable, and only from allowedSshSource.
//
//   az group create -n blorgfs-guest-host -l <region>
//   az deployment group create -g blorgfs-guest-host -f main.bicep \
//     -p sshPublicKey="$(cat ~/.ssh/id_ed25519.pub)" allowedSshSource=<your-ip>/32
//
// Then on the VM: clone BlorgFS, run ci/guest/image/build-image.sh once,
// and ci/guest/host/guestctl / ci/guest/run-guest-tests.sh from there on.
// Deallocate it when idle (az vm deallocate); the disk keeps the image.

@description('Location for all resources.')
param location string = resourceGroup().location

@description('VM size. Must support nested virtualization (Dv5/Dsv5/Ev5 families do).')
param vmSize string = 'Standard_D4s_v5'

@description('Admin user name on the host.')
param adminUsername string = 'blorg'

@description('SSH public key for the admin user. Password login is disabled.')
param sshPublicKey string

@description('CIDR allowed to reach SSH, e.g. your public IP as x.x.x.x/32. There is no default on purpose.')
param allowedSshSource string

@description('OS disk size in GB: the golden image, a run overlay and the ISOs need ~60 GB.')
param osDiskSizeGB int = 128

param namePrefix string = 'blorgfs-guest-host'

resource nsg 'Microsoft.Network/networkSecurityGroups@2023-11-01' = {
  name: '${namePrefix}-nsg'
  location: location
  properties: {
    securityRules: [
      {
        name: 'ssh-from-allowed-source'
        properties: {
          priority: 100
          direction: 'Inbound'
          access: 'Allow'
          protocol: 'Tcp'
          sourceAddressPrefix: allowedSshSource
          sourcePortRange: '*'
          destinationAddressPrefix: '*'
          destinationPortRange: '22'
        }
      }
    ]
  }
}

resource vnet 'Microsoft.Network/virtualNetworks@2023-11-01' = {
  name: '${namePrefix}-vnet'
  location: location
  properties: {
    addressSpace: { addressPrefixes: [ '10.80.0.0/24' ] }
    subnets: [
      {
        name: 'default'
        properties: {
          addressPrefix: '10.80.0.0/26'
          networkSecurityGroup: { id: nsg.id }
        }
      }
    ]
  }
}

resource pip 'Microsoft.Network/publicIPAddresses@2023-11-01' = {
  name: '${namePrefix}-ip'
  location: location
  sku: { name: 'Standard' }
  properties: { publicIPAllocationMethod: 'Static' }
}

resource nic 'Microsoft.Network/networkInterfaces@2023-11-01' = {
  name: '${namePrefix}-nic'
  location: location
  properties: {
    ipConfigurations: [
      {
        name: 'ipconfig1'
        properties: {
          subnet: { id: vnet.properties.subnets[0].id }
          privateIPAllocationMethod: 'Dynamic'
          publicIPAddress: { id: pip.id }
        }
      }
    ]
  }
}

resource vm 'Microsoft.Compute/virtualMachines@2024-03-01' = {
  name: namePrefix
  location: location
  properties: {
    hardwareProfile: { vmSize: vmSize }
    osProfile: {
      computerName: 'blorgfs-host'
      adminUsername: adminUsername
      customData: base64(loadTextContent('cloud-init.yaml'))
      linuxConfiguration: {
        disablePasswordAuthentication: true
        ssh: {
          publicKeys: [
            {
              path: '/home/${adminUsername}/.ssh/authorized_keys'
              keyData: sshPublicKey
            }
          ]
        }
      }
    }
    storageProfile: {
      imageReference: {
        publisher: 'Canonical'
        offer: 'ubuntu-24_04-lts'
        sku: 'server'
        version: 'latest'
      }
      osDisk: {
        createOption: 'FromImage'
        diskSizeGB: osDiskSizeGB
        managedDisk: { storageAccountType: 'Premium_LRS' }
        deleteOption: 'Delete'
      }
    }
    // No securityProfile, so Standard security: nothing on the host needs
    // Trusted Launch, and leaving it out avoids its restrictions on which
    // sizes and features (nested virtualization among them) can combine.
    networkProfile: {
      networkInterfaces: [ { id: nic.id, properties: { deleteOption: 'Delete' } } ]
    }
  }
}

output sshCommand string = 'ssh ${adminUsername}@${pip.properties.ipAddress}'
