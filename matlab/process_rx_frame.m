function [data_phase_corr, data_sc_idx, evm, snr] = process_rx_frame(configs, tx_pilot_cxdouble, tx_data_cxdouble, rx_pilot_cxdouble, rx_data_cxdouble)

% configs HAS BEEN DEFINED in inspect.m, which is following:
%     configs = [samples_per_slot tx_zero_prefix_len data_size data_start data_stop fft_size cp_len ...
%         total_dl_symbols dl_pilot_symbols total_users];
    
    %samples_per_slot = configs(1);
    tx_zero_prefix_len = configs(2);
    data_size = configs(3);
    %data_start = configs(4);
    %data_stop = configs(5);
    fft_size = configs(6);
    cp_len = configs(7);
    total_dl_symbols = configs(8);
    dl_pilot_symbols = configs(9);
    total_users = configs(10);     % which was set as "total_users = 1;" in inspect.m
    dl_data_symbols = total_dl_symbols - dl_pilot_symbols;

    
    %% Process Rx Data (Channel Estimation and Equalization)
    % Plot Constellations and print EVMs and SNRs
    start_id = cp_len + tx_zero_prefix_len;
    snr = zeros(1, total_users);
    evm = zeros(1, total_users);
    cl = 0;   % UNUSED variable
    nz_start_idx = (fft_size - data_size)/2;
    nz_sc_idx = nz_start_idx+1:nz_start_idx+data_size;
    clear nz_start_idx;
    plt_trk_sp = 16;
    data_sc_idx = setdiff(1:data_size, 1:plt_trk_sp:data_size);
    data_phase_corr = zeros(data_size, dl_data_symbols, total_users);
    for u=1:total_users
        % Process pilots
        ch_est = zeros(data_size, dl_pilot_symbols);
        for p=1:dl_pilot_symbols
          rx_pilot_f_tmp = fftshift(fft(rx_pilot_cxdouble(start_id + 1:start_id + fft_size, u, p)));
          ch_est(:, p) = rx_pilot_f_tmp(nz_sc_idx) ./ tx_pilot_cxdouble(:, u, p);
        end
        clear p
        if dl_pilot_symbols == 1
            ch_est_mean = ch_est;
        else
            ch_est_mean = mean(ch_est, 2);
        end

        %Process data symbols
        %data_phase_corr = zeros(data_size, dl_data_symbols);
        aevms = zeros(u, dl_data_symbols);
        for d=1:dl_data_symbols    % which is 2 in our case
          rx_data_f_tmp = fftshift(fft(rx_data_cxdouble(start_id + 1:start_id + fft_size, u, d)));
          data_eq = rx_data_f_tmp(nz_sc_idx) ./ ch_est_mean;

          % pilot tracking
          phase_err = angle(mean((data_eq(1:plt_trk_sp:end) .* conj(tx_pilot_cxdouble(1:plt_trk_sp:end, u)))));
          data_phase_corr(data_sc_idx, d, u) = data_eq(data_sc_idx) .* exp(-1j*phase_err);
          
          %----
          % Done channel equalization of this 'D'. Plot confusion matrix
          % for this 'D' in the loop
          tx_dataonly = tx_data_cxdouble(data_sc_idx, u, d);
          rx_dataonly = data_phase_corr(data_sc_idx, d, u);
          tx_demod = demod_sym(tx_dataonly, 16); % 16 stands for 16-QAM
          rx_demod = demod_sym(rx_dataonly, 16);
          figure;
          % cm = confusionchart(tx_demod, rx_demod, 'Normalization','row-normalized');
          cm = confusionchart(tx_demod, rx_demod);
          set(gca, 'FontSize',20);
          cm.XLabel = 'Received Symbols';
          cm.YLabel = 'Sent Symbols';
          cm.FontSize=20;
          cm.GridVisible = 'off';
          cm.RowSummary = 'row-normalized';
          cm.ColumnSummary = 'absolute';

          cm.Title = ['Error Matrix, Data Slot ', num2str(d)];
          %-----

          evm_mat = abs(data_phase_corr(data_sc_idx, d, u) - tx_data_cxdouble(data_sc_idx, u, d)).^2;
          aevms(u, d) = mean(evm_mat(:)); % needs to be a scalar

        end
        % clear d

        snr(u) = 10*log10(1./mean(aevms(u, :))); % calculate in dB scale.
        evm(u) = mean(aevms(u, :)) * 100;
        figure; plot(wrapTo2Pi(angle(ch_est_mean)), LineWidth=2); xlabel('SC Index'); ylabel('Phase [rad]'); grid on
        thisAxis=gca; thisAxis.FontSize=16;
        figure; plot(10.*log10((abs(ch_est_mean))), LineWidth=2); xlabel('SC Index'); ylabel('Magnitude [dB]'); grid on
        thisAxis=gca; thisAxis.FontSize=16;
    end



end
