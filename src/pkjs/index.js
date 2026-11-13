// disable all logging
console.log = function() {};

var APP_VERSION = "2.4";
var STORAGE_VERSION = 4;

var current_watch;
var current_config;
var watch_token;

// The configuration page is bundled with the app (see tools/build-config.js) and handed to the
// phone app as a data: URI, so no external server is needed.
var CONFIG_PAGE = require('./config-page');

Pebble.addEventListener('showConfiguration', function(e) {
  // Values the page reads instead of URL query parameters
  var params = {
    current_version: APP_VERSION,
    current_language: navigator.language,
    current_config: JSON.stringify(current_config),
    current_watch: JSON.stringify(current_watch)
  };
  var injected = '<script>window.__bcdParams = ' +
      JSON.stringify(params).replace(/</g, '\\u003c') + ';</script>';
  var html = CONFIG_PAGE.replace('<!--BCD-PARAMS-->', injected);
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

// Listen for when the watchface is opened
Pebble.addEventListener('ready',
  function(e) {
    console.log('PebbleKit JS ready!');

    if(Pebble.getActiveWatchInfo) {
      try {
        current_watch = Pebble.getActiveWatchInfo();
      } catch(err) {
        current_watch = {
          platform: "basalt",
        };
      }
    } else {
      current_watch = {
        platform: "aplite",
      };
    }
    //current_watch.platform = "emery"; // for testing
    console.log('Using watch info: ' + JSON.stringify(current_watch));

    watch_token = Pebble.getWatchToken();
    console.log('Using watch token: ' + watch_token);

    //localStorage.clear(); // for testing
    current_config = JSON.parse(localStorage.getItem(watch_token));
    if(undefined === current_config || null === current_config) {
      console.log('No watch specific config found, trying to load legacy storage');
      current_config = JSON.parse(localStorage.getItem(0));
    }
    if(undefined === current_config || null === current_config) {
      current_config = {};
      console.log('No config found');
    } else {
      console.log('Config found: ' + JSON.stringify(current_config));
      switch(current_config.storage_version) {
        case 2:
          if(current_watch.platform === "aplite") {
            if(current_config.displayfalse === 1) {
              current_config.displayfalse = 2;
            }
          }
          console.log('Config converted: ' + JSON.stringify(current_config));
          // no break
        case 3:
          if(current_watch.platform === "aplite") {
            if(current_config.displayfalse === 1) {
              current_config.displayfalse = 4;
            }
          }
  				current_config.showhint = current_config.tapping;
          delete current_config.tapping;
          current_config.btloss = (current_config.btloss === "vibrateandicon" || current_config.btloss === "icon") ? 1 : 0;
          current_config.btlevel = (current_config.btlevel === "vibrateandicon" || current_config.btlevel === "icon") ? 1 : 0;
          console.log('Config converted: ' + JSON.stringify(current_config));
          current_config.storage_version = STORAGE_VERSION;
          localStorage.setItem(watch_token, JSON.stringify(current_config));
          localStorage.removeItem(0);
      }
    }
    current_config.storage_version = STORAGE_VERSION;

    var dictionary = {
      'READY': 1,
    };
    // Send to Pebble
    Pebble.sendAppMessage(dictionary,
      function(e) {
        console.log('READY sent to Pebble successfully!');
      },
      function(e) {
        console.log('Error sending READY to Pebble!');
      }
    );
  }
);

Pebble.addEventListener('webviewclosed',
  function(e) {
    var configuration = JSON.parse(decodeURIComponent(e.response));
    console.log('Configuration window returned: ' + JSON.stringify(configuration));
    configuration.storage_version = STORAGE_VERSION;
    localStorage.setItem(watch_token, JSON.stringify(configuration));
    current_config = configuration;

    // Assemble dictionary using our keys
    var dictionary = {
      'TRUE_BINARY': configuration.truebinary,
      'DISPLAY_FALSE': configuration.displayfalse,
      'DISPLAY_IMPOSSIBLE' : configuration.showimpossible,
      'BACKGROUND_COLOR': parseInt("0x" + configuration.backgroundcolor),
      'TRUE_COLOR': parseInt("0x" + configuration.bitcolor),
      'FALSE_COLOR': parseInt("0x" + configuration.offcolor),
      'HINT_COLOR': parseInt("0x" + configuration.hintcolor),
      'DISPLAY_SECONDS': configuration.displayseconds,
      'TIME_ALIGNMENT' : configuration.timealignment,
      'TIME_FORMAT' : configuration.timeformat,
      'TIME_HINT': configuration.showhint,
      'DATE_TRUE_COLOR': parseInt("0x" + configuration.datebitcolor),
      'DATE_FALSE_COLOR': parseInt("0x" + configuration.dateoffcolor),
      'DATE_HINT_COLOR': parseInt("0x" + configuration.datehintcolor),
      'SHOW_DATE' : configuration.showdate,
      'DATE_POS' : configuration.dateposition,
      'DATE_ALIGNMENT' : configuration.datealignment,
      'DATE_FORMAT' : configuration.dateformat,
      'DATE_HINT': configuration.dateshowhint,
      'BLUETOOTH_ICON': configuration.btloss,
      'BLUETOOTH_VIBRATE': configuration.btlossvibration,
      'BLUETOOTH_RECONNECT_VIBRATE' : configuration.btreconnectvibration,
      'BATTERY_ICON': configuration.btlevel,
      'BATTERY_VIBRATE': configuration.btlevelvibration,
      'VIBRATE_HOURLY': configuration.vibratehourly,
      'VIBRATE_HOURLY_ASLEEP': configuration.vibratehourlyasleep || 0,
      'NO_VIBE_CHARGING': configuration.dndwhilecharging,
      'TAPPING_EFFECT_TIMEOUT': parseInt(configuration.tappingtimeout),
      'TIME_DECO': configuration.timedeco,
      'DATE_DECO': configuration.datedeco,
      'TRUE_BORDER': configuration.trueborder,
      'FALSE_BORDER': configuration.falseborder,
      'SHOW_SECONDS_INTERVAL': configuration.secint,
      'QUICK_VIEW_MODE': configuration.quickview,
    };

    console.log('Configuration send to Pebble: ' + JSON.stringify(dictionary));

    // Send to Pebble
    Pebble.sendAppMessage(dictionary,
      function(e) {
        console.log('Configuration sent to Pebble successfully!');
      },
      function(e) {
        console.log('Error sending configuration to Pebble!');
      }
    );
  }
);

// Listen for when an AppMessage is received
Pebble.addEventListener('appmessage',
  function(e) {
    console.log('AppMessage received: ' + JSON.stringify(e.payload));
    if('TIME_FORMAT' in e.payload) {
      current_config.watchtimeformat = e.payload.TIME_FORMAT;
      localStorage.setItem(watch_token, JSON.stringify(current_config));
    }
  }
);



//////////////////
// WEBPACK FOOTER
// ./src/pkjs/index.js
// module id = 2
// module chunks = 0
