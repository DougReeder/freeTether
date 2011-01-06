function MainAssistant() {

  this.DEFAULT_NETWORK = 'freeTether';
  this.DEFAULT_SECURITY = 'Open';
	this.WIFI_IFNAME = "uap0";
  this.BT_IFNAME = "bsl0";
  this.USB_IFNAME = "usb0:1";

  this.statusSubscription = null;
     
  this.ifToggle = {
    wifi: {value: false},
    bluetooth: {value: false},
    usb: {value: false}
  }
  
  this.ifSpinner = {
    wifi: {spinning: false},
    bluetooth: {spinning: false},
    usb: {spinning: false}
  }
	
  this.clientListModel = {items: [], "listTitle": $L("Connected Devices")};

}

MainAssistant.prototype.setup = function() {

	this.controller.document.body.className = 'palm-dark';
	
	this.dhcpIcon = this.controller.get('dhcp-icon');
	this.ipForwardIcon = this.controller.get('ip-forward-icon');

	this.toggles = this.controller.get('toggles');
	this.toggles.style.display = '';
	
	this.connections = this.controller.get('connections');
  this.connections.style.display = 'none';
	
	this.controller.listen('wifi-row', Mojo.Event.tap, this.ifaceRowTapped.bindAsEventListener(this, 'wifiPrefs'));
	this.controller.listen('bt-row', Mojo.Event.tap, this.ifaceRowTapped.bindAsEventListener(this, 'btPrefs'));
	this.controller.listen('usb-row', Mojo.Event.tap, this.ifaceRowTapped.bindAsEventListener(this, 'usbPrefs'));

  this.clientList          = this.controller.get('clientList');
	  
	this.controller.setupWidget(
		'wifi',
		{
      trueLabel:  'On',
      falseLabel: 'Off',
      fieldName:  'value'
		},
		this.ifToggle.wifi
	);

	this.controller.setupWidget(
		'bluetooth',
		{
      trueLabel:  'On',
      falseLabel: 'Off',
      fieldName:  'value'
		},
		this.ifToggle.bluetooth
	);
	
	this.controller.setupWidget(
		'usb',
		{
      trueLabel:  'On',
      falseLabel: 'Off',
      fieldName:  'value'
		},
		this.ifToggle.usb
	);
	
	this.controller.setupWidget(
    Mojo.Menu.appMenu,
    {
      omitDefaultItems: true
    },
    {
      visible: true,
      items: [
        {
          label: "Help",
          command: 'do-help'
        }
      ]
    }
  );

	this.controller.setupWidget(
		Mojo.Menu.viewMenu,
		{
			menuClass: 'no-fade'
		},
		{
			visible:true,
			items: [
				{},
				{label:$L('Views'), 
				toggleCmd:'toggles',
				items:[
					{label:$L('Interfaces'), command:'toggles', width:160},
					{label:$L('Connections'), command:'connections', width:160}, 
				]},
				{},
			]
		}
	);
	
  this.controller.setupWidget(
   'clientList',
    {
      itemTemplate: 'templates/device-item',
      listTemplate: 'templates/device-list',
    }, this.clientListModel);

	this.toggleChangeHandler = this.toggleChanged.bindAsEventListener(this);
	
	this.controller.listen('wifi', Mojo.Event.propertyChange, this.toggleChangeHandler);
	this.controller.listen('bluetooth', Mojo.Event.propertyChange, this.toggleChangeHandler);
	this.controller.listen('usb', Mojo.Event.propertyChange, this.toggleChangeHandler);
	 
  this.serviceStatusSubscription = FreeTetherService.monitorServer("org.webosinternals.freetether", this.server.bind(this));
  this.sysInfoSubscription = FreeTetherService.getStatus({subscribe: true}, this.handleSysInfo.bind(this));
  this.clientListSubscription = FreeTetherService.getClientList({subscribe: true}, this.handleClientList.bind(this));
	this.ipForwardSubscription = FreeTetherService.getIPForward({subscribe: true}, this.handleIPForward.bind(this));
	
	this.controller.setupWidget('wifiSpinner', {}, this.ifSpinner.wifi);
	this.controller.setupWidget('btSpinner', {}, this.ifSpinner.bluetooth);
	this.controller.setupWidget('usbSpinner', {}, this.ifSpinner.usb);
	
};

MainAssistant.prototype.server = function(payload) {
        for (p in payload) {
          Mojo.Log.error(p + " : " + payload[p]);
        }
}

MainAssistant.prototype.addNewClients = function(clients) {
  var client = {};
  var found = false;

  for (var i=0; i<clients.length; i++) {
    found = false;
    client = clients[i];
    for (var j=0; j<this.clientListModel.items.length; j++) {
      if (client.mac === this.clientListModel.items[j].mac && client.ipv4) {
        found = true;
        break;
      }
    }

    if (found) {
      if (!this.clientListModel.items[j].type != client.type)
        this.clientListModel.items[j].type = client.type || "usb";
      if (this.clientListModel.items[j].hostname != client.hostname)
        this.clientListModel.items[j].hostname = client.hostname || client.mac;
      if (this.clientListModel.items[j].mac != client.mac)
        this.clientListModel.items[j].mac = client.mac;
      if (this.clientListModel.items[j].ipv4 != client.ipv4)
        this.clientListModel.items[j].ipv4 = client.ipv4;
    }
    else {
      var newClient = {};
      newClient.type = client.type || "usb";
      newClient.name = client.hostname || client.mac;
      newClient.mac = client.mac;
      newClient.ip = client.ipv4;
      if (this.clientListModel.items.length && this.clientListModel.items[0].empty)
        this.clientListModel.items.clear();
      this.clientListModel.items.push(newClient);
    }
  }
}

MainAssistant.prototype.removeOldClients = function(clients) {
  var client = {};
  var found = false;

  for (var i=0; i<this.clientListModel.items.length; i++) {
    found = false;
    client = this.clientListModel.items[i];
    for (var j=0; j<clients.length; j++) {
      if (client.mac === clients[j].mac) {
        found = true;
        break;
      }
    }

    if (!found) {
      this.clientListModel.items = this.clientListModel.items.without(client);
    }
  }
}

MainAssistant.prototype.handleClientList = function(payload) {
  if (payload && payload.clients) {
    this.addNewClients(payload.clients);
    this.removeOldClients(payload.clients);
  }
  else {
    this.clientListModel.items.clear();
  }

  if (!this.clientListModel.items.length)
    this.clientListModel.items.push({"empty": true});

  this.controller.modelChanged(this.clientListModel, this);
}

MainAssistant.prototype.handleIPForward = function(payload) {

  if (!payload || typeof(payload.state) !== 'number')
    return;
  
  var ipforwardClass = '';
  switch (payload.state) {
    case 0:
      ipforwardClass = 'icon status-offline';
      break;
    case 1:
      ipforwardClass = 'icon status-available';
      break;
  }
  this.ipForwardIcon.className = ipforwardClass;

}

MainAssistant.prototype.handleSysInfo = function(payload) {

  if (!payload || !payload.sysInfo)
    return;
  
  var dhcpClass = '';
  switch (payload.sysInfo.stateDHCPServer) {
    case 'STOPPED':
      dhcpClass = 'icon status-offline';
      break;
    case 'STARTED':
      dhcpClass = 'icon status-available';
      break;
    case 'START REQUESTED':
    case 'STOP REQUESTED':
      dhcpClass = 'icon status-busy';
      break;
    case 'ERROR':
      dhcpClass = 'icon status-error';
      break;
  }
  this.dhcpIcon.className = dhcpClass;
 
  var interfaces = ['wifi','bluetooth','usb'];
  var len = payload.sysInfo.interfaces.length;
  var i = 0;
  
  ['wifi','bluetooth','usb'].each(function(ifType){
    this.ifSpinner[ifType].spinning = false;
    this.ifToggle[ifType].value = false;
  }, this);
  
  if (len > 0) {
    for (; i<len; i++) {
      ifType = payload.sysInfo.interfaces[i].type;
      ifState = payload.sysInfo.interfaces[i].stateInterface;
      if (ifState == 'CREATE REQUESTED' || ifState == 'DESTROY REQUESTED')
        this.ifSpinner[ifType].spinning = true;
      if (ifState == 'CREATED' || ifState == 'CREATE REQUESTED')
        this.ifToggle[ifType].value = true;
    }
  }
  
  ['wifi','bluetooth','usb'].each(function(ifType){
    this.controller.modelChanged(this.ifSpinner[ifType], this);
    this.controller.modelChanged(this.ifToggle[ifType], this);
  }, this);

}

MainAssistant.prototype.handleCommand = function(event) {
	
	if(event.type == Mojo.Event.command) {
		
		switch (event.command)
		{
			case 'toggles':
				this.toggles.style.display = '';
				this.connections.style.display = 'none';
				this.controller.sceneScroller.mojo.revealTop()
				break;
				
     case 'connections':
        this.toggles.style.display = 'none';
        this.connections.style.display = '';
        this.controller.sceneScroller.mojo.revealTop()
        break;
				
      case 'do-help':
        this.controller.stageController.pushScene('help');
        break;

		}
			
	}
	
}

MainAssistant.prototype.addInterface = function(type) {
  var payload = {};
  payload[type] = {};

  switch(type) {
    case 'wifi':
      payload[type].SSID = prefs.get().network || this.DEFAULT_NETWORK;
      payload[type].Security = prefs.get().security || this.DEFAULT_SECURITY;
      
      if (payload[type].Security !== 'Open')
        payload[type].Passphrase = prefs.get().passphrase || "";

      //payload[type].interfaceIdleTimeout = true;
      break;
    case 'bluetooth':
      payload[type].ifname = this.BT_IFNAME;
      break;
    case 'usb':
      payload[type].ifname = this.USB_IFNAME;
      break;
  }
  
  FreeTetherService.addInterface(payload);
}

MainAssistant.prototype.removeInterface = function(type) {
  var payload = {};
  payload[type] = {};

  switch(type) {
    case 'wifi':
      payload[type].ifname = this.WIFI_IFNAME;
      break;
    case 'bluetooth':
      payload[type].ifname = this.BT_IFNAME;
      break;
    case 'usb':
      payload[type].ifname = this.USB_IFNAME;
      break;
  }

  FreeTetherService.removeInterface(payload);
}

MainAssistant.prototype.toggleChanged = function(event) {

  if (event.value) 
    this.addInterface(event.target.id);
  else
    this.removeInterface(event.target.id);

}

MainAssistant.prototype.ifaceRowTapped = function(event, item) {

  this.controller.stageController.pushScene(item);
  
}

MainAssistant.prototype.activate = function(event) {

};

MainAssistant.prototype.deactivate = function(event) {

};

MainAssistant.prototype.cleanup = function(event) {

};
