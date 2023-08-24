import {
    LitElement,
    html,
    css,
} from "https://unpkg.com/lit-element@2.0.1/lit-element.js?module";

const supportsButtonPressTileFeature = (stateObj) => {
    const domain = stateObj.entity_id.split(".")[0];
    return domain === "number";
};

class ButtonPressTileFeature extends LitElement {
    static get properties() {
        return {
            hass: undefined,
            config: undefined,
            stateObj: undefined,
        };
    }

    static getStubConfig() {
        return {
            type: "custom:number-desk-tile-feature",
            sitPosition: 420,
            standPosition: 3900,
        };
    }

    setConfig(config) {
        if (!config) {
            throw new Error("Invalid configuration");
        }
        this.config = config;
    }

    _sit(ev) {
        ev.stopPropagation();
        this.hass.callService("number", "set_value", {
            entity_id: this.stateObj.entity_id,
            value: this.config.sitPosition,
        });
    }

    _stand(ev) {
        ev.stopPropagation();
        this.hass.callService("number", "set_value", {
            entity_id: this.stateObj.entity_id,
            value: this.config.standPosition,
        });
    }

    render() {
        if (
            !this.config ||
            !this.hass ||
            !this.stateObj ||
            !supportsButtonPressTileFeature(this.stateObj)
        ) {
            return null;
        }

        return html`
        <ha-control-button-group>
            <ha-control-button .label=sit @click=${this._sit}>
                <ha-icon icon="mdi:arrow-down-bold"></ha-icon> Sit
            </ha-control-button>
            <ha-control-button .label=stand @click=${this._stand}>
                <ha-icon icon="mdi:arrow-up-bold"></ha-icon> Stand
            </ha-control-button>
        </ha-control-button-group>
      `;
    }

    static get styles() {
        return css`
        ha-control-button-group {
            margin: 0 12px 12px 12px;
            --control-button-group-spacing: 12px;
          }
      `;
    }
}

customElements.define("number-desk-tile-feature", ButtonPressTileFeature);

window.customTileFeatures = window.customTileFeatures || [];
window.customTileFeatures.push({
    type: "number-desk-tile-feature",
    name: "Desk Sit/Stand",
    supported: supportsButtonPressTileFeature,
    configurable: true,
});