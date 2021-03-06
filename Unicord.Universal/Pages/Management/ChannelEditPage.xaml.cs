﻿using System;
using System.Collections.Generic;
using DSharpPlus.Entities;
using Unicord.Universal.Models;
using Unicord.Universal.Pages.Management.Channel;
using Unicord.Universal.Services;
using Unicord.Universal.Utilities;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;
using NavigationView = Microsoft.UI.Xaml.Controls.NavigationView;
using NavigationViewBackRequestedEventArgs = Microsoft.UI.Xaml.Controls.NavigationViewBackRequestedEventArgs;
using NavigationViewItemInvokedEventArgs = Microsoft.UI.Xaml.Controls.NavigationViewItemInvokedEventArgs;

namespace Unicord.Universal.Pages.Management
{
    public sealed partial class ChannelEditPage : Page, IOverlay
    {
        private static readonly IReadOnlyDictionary<string, Type> TypeMap
            = new Dictionary<string, Type>()
            {
                ["Overview"] = typeof(ChannelEditOverviewPage),
                ["Permissions"] = typeof(ChannelEditPermissionsPage)
            };

        private ChannelEditViewModel _viewModel;

        public ChannelEditPage()
        {
            InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            if (e.Parameter is DiscordChannel channel)
            {
                // TODO: check perms
                _viewModel = new ChannelEditViewModel(channel);
                DataContext = _viewModel;
            }
        }

        private void Page_Loaded(object sender, RoutedEventArgs e)
        {            
            WindowManager.HandleTitleBarForControl(topGrid);
            WindowManager.HandleTitleBarForControl(navigationView, true);
        }

        private async void AcceptButton_Click(object sender, RoutedEventArgs e)
        {
            acceptButton.Visibility = Visibility.Collapsed;
            mainContent.IsEnabled = false;
            progressRing.IsActive = true;

            await _viewModel.SaveChangesAsync();

            progressRing.IsActive = false;
            OverlayService.GetForCurrentView().CloseOverlay();
        }

        private void BackButton_Click(object sender, RoutedEventArgs e)
        {
            OverlayService.GetForCurrentView().CloseOverlay();
        }

        private void NavigationView_BackRequested(NavigationView sender, NavigationViewBackRequestedEventArgs args)
        {
            OverlayService.GetForCurrentView().CloseOverlay();
        }

        private void NavigationView_ItemInvoked(NavigationView sender, NavigationViewItemInvokedEventArgs args)
        {
            if (args.InvokedItemContainer.Tag is string str)
            {
                if (TypeMap.TryGetValue(str, out var type))
                {
                    MainFrame.Navigate(type);
                }
            }
        }
    }
}
